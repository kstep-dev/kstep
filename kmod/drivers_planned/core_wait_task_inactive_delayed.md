# Core: wait_task_inactive() Stalls on sched_delayed Tasks

**Commit:** `b7ca5743a2604156d6083b88cefacef983f3a3a6`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.16-rc1
**Buggy since:** v6.12 (introduced by `152e11f6df29` "sched/fair: Implement delayed dequeue")

## Bug Description

The `wait_task_inactive()` function in `kernel/sched/core.c` is used to wait until a given task is no longer on any runqueue—i.e., it is fully dequeued and inactive. This function is called by `__kthread_bind_mask()` (via `kthread_bind()`) to ensure a kthread is truly stopped before changing its CPU affinity. The function is part of a critical path during boot and CPU hotplug, where `smpboot_create_threads()` creates and binds per-CPU kthreads.

Starting with v6.12, the EEVDF scheduler introduced "delayed dequeue" (`152e11f6df29`), a mechanism where tasks that go to sleep with negative lag (i.e., they consumed more CPU time than their fair share) are not immediately removed from the runqueue. Instead, they remain enqueued with `p->se.sched_delayed = 1`, continuing to "compete" in the virtual time space until their lag decays to zero, at which point they become eligible and are properly dequeued. This prevents tasks from gaming the scheduler by micro-sleeping at the end of their time quantum.

However, this delayed dequeue mechanism creates an unexpected interaction with `wait_task_inactive()`. When a newly created kthread briefly runs during its creation sequence (inside the `kthread()` wrapper function) and then goes to sleep in `TASK_UNINTERRUPTIBLE` state, it may accumulate enough runtime to have negative lag. With delayed dequeue enabled, this sleeping task remains on the runqueue as `sched_delayed`. When `kthread_bind()` subsequently calls `wait_task_inactive()`, the function sees the task as still "queued" via `task_on_rq_queued()` and enters its fallback path: sleeping for one scheduler tick (`NSEC_PER_SEC / HZ`) before retrying. Since `smpboot_create_threads()` creates multiple kthreads in succession, each incurring at least one tick of delay, the cumulative slowdown is substantial—making the boot-time thread creation path take much longer in v6.12+ compared to v6.6.

## Root Cause

The root cause is a semantic mismatch between `wait_task_inactive()` and the delayed dequeue mechanism. In `wait_task_inactive()`, after acquiring the runqueue lock, the function checks whether the target task is still queued:

```c
rq = task_rq_lock(p, &rf);
trace_sched_wait_task(p);
running = task_on_cpu(rq, p);
queued = task_on_rq_queued(p);
```

The function `task_on_rq_queued(p)` returns true if the task is on the runqueue. With delayed dequeue, a task that has logically gone to sleep (its `__state` is `TASK_UNINTERRUPTIBLE`) can still be physically on the runqueue with `p->se.sched_delayed == 1`. From `wait_task_inactive()`'s perspective, this task appears "queued" even though it will never voluntarily run again—it is merely waiting for its lag to decay.

When `queued` is true, `wait_task_inactive()` enters its fallback path:

```c
if (unlikely(queued)) {
    ktime_t to = NSEC_PER_SEC / HZ;
    set_current_state(TASK_UNINTERRUPTIBLE);
    schedule_hrtimeout(&to, HRTIMER_MODE_REL_HARD);
    continue;
}
```

This sleeps the calling thread for one full tick (typically 1–10ms depending on `HZ`), then re-checks. On each re-check, if the sched_delayed task has not yet been naturally dequeued (which requires a scheduler tick to advance virtual time and check eligibility), the loop repeats. In practice, the sched_delayed task may persist across multiple iterations, as the dequeue only happens when the scheduler evaluates it during a tick on the CPU where it is enqueued.

The call chain that triggers this is:

```
smpboot_create_threads()
  → kthread_create_on_cpu()
    → kthread_bind()
      → __kthread_bind_mask()
        → wait_task_inactive(p, TASK_UNINTERRUPTIBLE)
```

During `kthread_create()`, the new kthread briefly runs inside the `kthread()` wrapper function to complete its initialization (signal creation, set up data structures). This brief execution gives the task some vruntime. In EEVDF, new tasks start at the current `avg_vruntime`. Any execution pushes their actual vruntime beyond their virtual deadline, giving them negative lag. When the kthread then sets its state to `TASK_UNINTERRUPTIBLE` and calls `schedule()`, the delayed dequeue mechanism kicks in: since the task has negative lag, it is kept on the runqueue as `sched_delayed` rather than being fully dequeued.

Peter Zijlstra confirmed this analysis in the LKML discussion, noting "they start at 0, any runtime will likely push them negative" (referring to lag), and Phil Auld observed this seemed surprising for newly created tasks.

## Consequence

The primary consequence is a significant performance regression in boot time and CPU hotplug operations. The `smpboot_create_threads()` function creates one kthread per CPU for each registered smpboot thread type (e.g., ksoftirqd, migration, cpuhp). On a system with many CPUs (e.g., 128 or 256 cores), this means hundreds of kthread creations, each potentially stalling for one or more tick periods in `wait_task_inactive()`. With `HZ=1000`, each tick is 1ms; with `HZ=250`, each tick is 4ms. For 256 CPUs with ~10 smpboot thread types, that is 2560 kthread bind operations, each potentially costing 1–4ms, totaling 2.5–10 seconds of additional boot delay.

This regression was reported by peter-yc.chang@mediatek.com, who observed that `smpboot_create_threads()` was taking "much longer" in v6.12 compared to v6.6. John Stultz confirmed the issue and narrowed it down to the `wait_task_inactive()` call path. Disabling the `DELAY_DEQUEUE` scheduler feature (via the sched features debugfs interface) recovered the performance, confirming the delayed dequeue mechanism as the cause.

The bug does not cause crashes, data corruption, or incorrect scheduling decisions. It is purely a performance issue affecting latency-sensitive boot and hotplug paths. However, for systems where fast boot or rapid CPU online/offline is important (e.g., embedded systems, cloud instances, power management), this regression is significant.

## Fix Summary

The fix adds a simple check at the beginning of `wait_task_inactive()`'s locked section: if the target task has `p->se.sched_delayed` set, it forces the task to be dequeued immediately by calling `dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_DELAYED)`:

```c
rq = task_rq_lock(p, &rf);
/*
 * If task is sched_delayed, force dequeue it, to avoid always
 * hitting the tick timeout in the queued case
 */
if (p->se.sched_delayed)
    dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
trace_sched_wait_task(p);
running = task_on_cpu(rq, p);
queued = task_on_rq_queued(p);
```

The `DEQUEUE_SLEEP` flag indicates the task is being dequeued because it is sleeping, and `DEQUEUE_DELAYED` flag tells the scheduler that this is a force-dequeue of a delayed task. After this forced dequeue, `task_on_rq_queued(p)` returns false, allowing `wait_task_inactive()` to recognize the task as truly inactive and return immediately.

This fix is correct because a `sched_delayed` task is logically asleep—it will not run again until explicitly woken. The delayed dequeue is an optimization for EEVDF lag management, not a requirement for correctness. Force-dequeueing a sched_delayed task simply completes the dequeue early, forfeiting any remaining lag decay benefit. For a task that is about to have its CPU affinity forcibly changed (the whole purpose of `kthread_bind()` → `wait_task_inactive()`), this is entirely safe and desirable.

Peter Zijlstra considered an alternative approach—simply treating sched_delayed tasks as not-queued by modifying the `queued` assignment to `queued = task_on_rq_queued() && !p->se.sched_delayed`—but noted this was "pushing things quite far" as it would mean changing `cpus_allowed` while the task is still physically enqueued, which is "fairly tricky and not worth the mental pain." The explicit dequeue approach is simpler and safer.

## Triggering Conditions

The following conditions are required to trigger this bug:

1. **Kernel version v6.12 or later** with the delayed dequeue feature enabled (the `DELAY_DEQUEUE` sched feature must be active, which is the default). The bug was introduced by commit `152e11f6df29` merged in v6.12-rc1.

2. **Any call to `wait_task_inactive()` on a task that is in `sched_delayed` state.** The most common trigger path is `kthread_bind()` → `__kthread_bind_mask()` → `wait_task_inactive(p, TASK_UNINTERRUPTIBLE)` on a newly created (but not yet started) kthread. This happens during `smpboot_create_threads()` at boot time and during CPU hotplug.

3. **The target task must have accumulated negative lag** before going to sleep. For newly created kthreads, this happens naturally: the kthread runs briefly inside the `kthread()` wrapper during creation, consuming some CPU time. In EEVDF, any execution by a newly placed entity pushes its vruntime past its deadline, resulting in negative lag.

4. **No specific CPU count or topology is required**, but the performance impact scales with the number of CPUs because `smpboot_create_threads()` creates kthreads proportional to CPU count. Even a 2-CPU system will exhibit the delay, though it will be less noticeable. The bug is deterministic—every kthread_bind on a sched_delayed task will hit the 1-tick timeout.

5. **No special kernel configuration beyond default is needed.** The delayed dequeue feature is enabled by default in v6.12+. `CONFIG_SMP=y` is required (for `wait_task_inactive()` to exist and for smpboot threads to be created).

The bug is highly reliable and deterministic. Any system running kernel v6.12 through v6.15 will experience this delay during boot (via smpboot_create_threads) and during CPU hotplug operations. The probability of reproduction is essentially 100% on affected kernel versions.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP by creating kthreads and calling the kernel's `kthread_bind()` function (which is `EXPORT_SYMBOL` and directly callable from a kernel module), then measuring the wall-clock time it takes to complete. On a buggy kernel, each `kthread_bind()` call will stall for at least one scheduler tick due to the sched_delayed task appearing "queued" in `wait_task_inactive()`. On a fixed kernel, the call returns immediately because the sched_delayed task is force-dequeued.

### Step-by-step plan:

1. **QEMU configuration:** At least 2 CPUs (to have a CPU other than CPU 0 for kthread binding). No special topology, memory, or NUMA configuration needed.

2. **Kernel version guard:** The driver should be guarded with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)` since the delayed dequeue feature (and thus the bug) was introduced in v6.12.

3. **Task creation:** Create several kthreads using `kstep_kthread_create()`. This internally calls `kthread_create()`, which creates a kthread that briefly runs (inside the `kthread()` wrapper) and then sleeps in `TASK_UNINTERRUPTIBLE`. With delayed dequeue, these newly created kthreads should end up in `sched_delayed` state. Create at least 4–8 kthreads to accumulate a measurable delay.

4. **Verification of sched_delayed state:** Before calling `kthread_bind()`, read `p->se.sched_delayed` for each created kthread to confirm it is indeed in the delayed state. Log this value. If it is 0, the task was already fully dequeued and the bug won't manifest for that particular task (tick a few times or try again).

5. **Time measurement:** Record `ktime_get_ns()` before and after each `kthread_bind(p, cpu)` call. On a buggy kernel, each call where the task is sched_delayed should take at least `NSEC_PER_SEC / HZ` nanoseconds (1ms at HZ=1000, 4ms at HZ=250). On a fixed kernel, it should take only microseconds.

6. **Call kthread_bind:** Call `kthread_bind(p, 1)` directly from the driver for each created kthread. This function is exported by the kernel (`EXPORT_SYMBOL(kthread_bind)`) and can be called from any kernel module. It will internally call `__kthread_bind_mask()` → `wait_task_inactive(p, TASK_UNINTERRUPTIBLE)`, triggering the bug path.

7. **Detection criteria:**
   - **Pass (bug detected on buggy kernel):** The total time for all `kthread_bind()` calls exceeds `N * (NSEC_PER_SEC / HZ / 2)` where N is the number of sched_delayed tasks. Individual calls that find sched_delayed tasks should each take ≥ 500µs.
   - **Pass (bug fixed on fixed kernel):** The total time for all `kthread_bind()` calls is well under 1ms (microsecond range), and no individual call takes more than 100µs.
   - Use `kstep_pass()` / `kstep_fail()` to report results based on whether the observed timing matches the expected behavior for the kernel version.

8. **Cleanup:** After measurement, start the kthreads with `kstep_kthread_start()` or let them be cleaned up normally.

9. **Expected behavior:**
   - **Buggy kernel (v6.12–v6.15):** Each `kthread_bind()` call on a sched_delayed kthread takes at least 1 tick (1–4ms). Creating and binding 8 kthreads should take at least 8–32ms total. The `p->se.sched_delayed` field will be 1 before bind and 1 after bind (the buggy kernel never clears it in wait_task_inactive).
   - **Fixed kernel (v6.16+):** Each `kthread_bind()` call returns in microseconds. The total time for 8 binds is well under 1ms. The `p->se.sched_delayed` field may be 1 before bind but 0 after bind (the fix force-dequeues the task).

10. **Alternative detection method:** Instead of pure timing, we can also check the `task_on_rq_queued()` status and `p->se.sched_delayed` before and after the bind call. On the buggy kernel, after `kthread_bind()` returns (which it eventually does after the tick timeout), the task may still be sched_delayed (if the scheduler hasn't naturally dequeued it). On the fixed kernel, `p->se.sched_delayed` should be 0 after the bind because `wait_task_inactive()` force-dequeued it.

### Required kSTEP capabilities:
- `kstep_kthread_create()` — already available
- Direct call to kernel's `kthread_bind()` — available because it is `EXPORT_SYMBOL`
- `ktime_get_ns()` — standard kernel API, available in any module
- Access to `p->se.sched_delayed` — accessible via kSTEP's internal.h (scheduler internals)
- `kstep_kthread_start()` — already available (for cleanup)
- No KSYM_IMPORT needed; all required functions are either part of kSTEP's API or are exported kernel symbols

No modifications to kSTEP framework are needed. All required functionality is already available.
