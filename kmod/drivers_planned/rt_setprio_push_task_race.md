# RT: rt_mutex_setprio() vs push_rt_task() Race Causes BUG in convert_prio()

**Commit:** `49bef33e4b87b743495627a529029156c6e09530`
**Affected files:** `kernel/sched/rt.c`, `kernel/sched/deadline.c`
**Fixed in:** v5.18-rc1
**Buggy since:** v5.11-rc1 (commit `a7c81556ec4d` — "sched: Fix migrate_disable() vs rt/dl balancing")

## Bug Description

A race condition exists between `rt_mutex_setprio()` and `push_rt_task()` in the RT scheduler's push-migration logic. When a task that was temporarily boosted to RT priority via rt_mutex priority inheritance releases the mutex and gets demoted back to CFS (SCHED_NORMAL), the local CPU may still receive an `rto_push_irq_work` interrupt before the CPU has a chance to reschedule. When `push_rt_task()` runs in this state, it encounters `rq->curr` as a CFS task (with `p->prio == 120`), and if the highest-priority pushable RT task has migration disabled, the function calls `find_lowest_rq(rq->curr)` on a non-RT task. This call chain descends into `cpupri_find_fitness()` → `convert_prio()`, where the CFS priority value 120 is not handled by the switch statement, triggering a `BUG()`.

The bug was introduced by commit `a7c81556ec4d` ("sched: Fix migrate_disable() vs rt/dl balancing"), which added the `is_migration_disabled(next_task)` branch to `push_rt_task()`. This branch was designed to handle the case where the highest-priority pushable RT task has migration disabled: instead of pushing that task, it tries to push `rq->curr` to a different CPU by calling `find_lowest_rq(rq->curr)`. The implicit assumption was that `rq->curr` would always be an RT task when this code path executes, since the runqueue is RT-overloaded. However, this assumption breaks when `rq->curr` gets demoted from RT to CFS between the time the overload state was set and when `push_rt_task()` actually runs.

The bug was originally reported by John Keeping on the linux-rt-users mailing list, observed on v5.15.10-rt24 running on ARM hardware (Rockchip). The crash manifested as `kernel BUG at kernel/sched/cpupri.c:151!` with `r0 : 00000078` (0x78 = 120, the normal CFS priority value). Dietmar Eggemann independently reproduced it on an ARM64 Juno-r0 board with v5.15.10-rt24 and CONFIG_PREEMPT_RT=y. The bug occurred approximately half the time during userspace startup, affecting various processes (systemd-udevd, pr/ttyS2) that happened to trigger rt_mutex contention.

The fix, authored by Valentin Schneider, moves the priority comparison check (`next_task->prio < rq->curr->prio`) earlier in `push_rt_task()` — before the `is_migration_disabled()` branch — and adds an explicit `rq->curr->sched_class != &rt_sched_class` guard before calling `find_lowest_rq(rq->curr)`. A parallel fix reorders checks in `push_dl_task()` for the SCHED_DEADLINE path.

## Root Cause

The root cause is a missing validation of `rq->curr`'s scheduling class in the `is_migration_disabled(next_task)` branch of `push_rt_task()`. The vulnerable code path in the buggy kernel is:

```c
static int push_rt_task(struct rq *rq, bool pull)
{
    ...
    next_task = pick_next_pushable_task(rq);
    ...
retry:
    if (is_migration_disabled(next_task)) {
        ...
        cpu = find_lowest_rq(rq->curr);  // BUG: rq->curr may not be RT!
        ...
    }
    ...
    // The priority check was here originally, too late:
    if (unlikely(next_task->prio < rq->curr->prio)) {
        resched_curr(rq);
        return 0;
    }
    ...
}
```

The race sequence proceeds as follows:

1. **Setup**: CPU X has multiple RT tasks queued, making it RT-overloaded (`rq->rt.overloaded == true`). One of these RT tasks (call it `T_migdis`) has migration disabled (`is_migration_disabled(T_migdis) == true`). The currently running task `T_curr` is also an RT task, but it is a CFS task that was temporarily boosted to RT priority through rt_mutex priority inheritance (PI). The CPU is in the `rto_mask` because it has `rt_nr_migratory >= 2`.

2. **Demotion**: `T_curr` releases the rt_mutex. The kernel calls `mark_wakeup_next_waiter()` → `rt_mutex_adjust_prio()` → `rt_mutex_setprio()`. Inside `rt_mutex_setprio()`, `T_curr`'s scheduling class is changed from `rt_sched_class` to `fair_sched_class`, and its priority is reset to its original CFS priority (e.g., 120 for `SCHED_NORMAL` nice 0). The function `check_class_changed()` is called, which invokes:
   - `switched_from_rt()`: This only calls `rt_queue_pull_task()` if `rq->rt.rt_nr_running == 0`. Since `T_migdis` (and potentially other RT tasks) are still queued, `rt_nr_running > 0`, so no pull is triggered. Crucially, the local CPU may **remain in the `rto_mask`** because it still has migratory RT tasks.
   - `switched_to_fair()`: This sets `TIF_NEED_RESCHED` on the current task, marking that a reschedule is needed.

3. **Balance callbacks and lock release**: `__balance_callbacks()` runs any queued balance callbacks, then `raw_spin_rq_unlock(rq)` releases the runqueue lock. At this point, `T_curr` is still the current task on CPU X (it hasn't been preempted yet), but it is now a CFS task with priority 120. `TIF_NEED_RESCHED` is set but the reschedule hasn't happened yet.

4. **IPI/irq_work arrival**: Before CPU X gets a chance to reschedule (i.e., before returning to the scheduler via `preempt_schedule_irq` or `schedule`), another CPU Y that is running `rto_push_irq_work_func()` selects CPU X as the next CPU in the RTO chain (via `rto_next_cpu(rd)`). CPU Y queues `rto_push_work` as irq_work on CPU X via `irq_work_queue_on(&rd->rto_push_work, cpu_X)`.

5. **Push execution**: CPU X receives the IPI, enters `rto_push_irq_work_func()`. This function checks `has_pushable_tasks(rq)` (true, since `T_migdis` is on the pushable list), acquires `rq->lock`, and calls `push_rt_task(rq, true)`.

6. **The crash path**: Inside `push_rt_task()`:
   - `pick_next_pushable_task(rq)` returns `T_migdis` (the migration-disabled RT task).
   - At the `retry:` label, `is_migration_disabled(T_migdis)` is true, so we enter the migration-disabled branch.
   - `pull` is true (called from `rto_push_irq_work_func`), `rq->push_busy` is false, so we proceed.
   - `find_lowest_rq(rq->curr)` is called. But `rq->curr == T_curr`, which is now a CFS task with `p->prio == 120`.
   - Inside `find_lowest_rq()`, `cpupri_find_fitness(&task_rqs(p)->rd->cpupri, p, ...)` is called.
   - Inside `cpupri_find_fitness()`, `convert_prio(p->prio)` is called with `prio == 120`.
   - `convert_prio()` only handles priorities `-1`, `0..98`, `99` (`MAX_RT_PRIO-1`), and `100` (`MAX_RT_PRIO`). Priority `120` has no matching case, so the function falls through the switch and returns an uninitialized `cpupri` value, or on architectures with strict switch handling, hits `BUG()` (as observed on ARM: `kernel BUG at kernel/sched/cpupri.c:151!`).

## Consequence

The primary consequence is a **kernel BUG/crash**. On ARM (32-bit), the crash manifests as:
```
kernel BUG at kernel/sched/cpupri.c:151!
Internal error: Oops - BUG: 0 [#1] PREEMPT_RT SMP ARM
```
The register dump shows `r0 : 00000078` (0x78 = 120), confirming that `convert_prio()` received a normal CFS priority instead of an RT priority. The call trace is:
```
cpupri_find_fitness
  find_lowest_rq
    push_rt_task.part.0
      rto_push_irq_work_func
        irq_work_single
          flush_smp_call_function_queue
            do_handle_IPI
```

On ARM64 (Juno-r0), the same bug manifests as a `WARNING` at `rt.c:1898` (the `WARN_ON(next_task == rq->curr)` check), with the diagnostic output showing `next_task=[rcu_preempt 11]` (SCHED_FIFO, rt_prio 1) and `rq->curr=[ksoftirqd/3 35]` (CFS, nice 0). This indicates that the current task on the CPU is a CFS task while the push mechanism is operating.

The bug is non-deterministic and depends on the precise timing of IPI delivery relative to the rt_mutex release path. John Keeping reported it occurring "maybe half the time as userspace is starting up" on his Rockchip board. Once the system booted past the initial startup phase (with heavy rt_mutex contention from console output and udev), the bug stopped occurring. The crash is fatal — it kills the affected process and may destabilize the entire system since it occurs in interrupt context (IPI handler).

## Fix Summary

The fix by Valentin Schneider makes two key changes to `push_rt_task()` in `kernel/sched/rt.c`:

**First**, the priority comparison check is moved from after the `is_migration_disabled()` branch to before it, immediately after the `retry:` label:
```c
retry:
    /* NEW: Check priority before entering migration-disabled branch */
    if (unlikely(next_task->prio < rq->curr->prio)) {
        resched_curr(rq);
        return 0;
    }

    if (is_migration_disabled(next_task)) {
        ...
    }
```
This ensures that if `rq->curr` has been demoted to a lower priority class (e.g., CFS priority 120 > any RT priority), the function immediately triggers a reschedule and returns, rather than attempting to find a CPU to push `rq->curr` to. Since any RT task's `prio` (0–98) is less than any CFS task's `prio` (100+), `next_task->prio < rq->curr->prio` will always be true when `rq->curr` is CFS, causing the early exit.

**Second**, an explicit scheduling class check is added inside the `is_migration_disabled()` branch, before the call to `find_lowest_rq(rq->curr)`:
```c
    if (is_migration_disabled(next_task)) {
        ...
        /* NEW: Don't call find_lowest_rq on non-RT curr */
        if (rq->curr->sched_class != &rt_sched_class)
            return 0;

        cpu = find_lowest_rq(rq->curr);
        ...
    }
```
This is a defense-in-depth check. The comment notes that stopper tasks masquerade as `SCHED_FIFO` (see `sched_set_stop_task()`), so the check uses `sched_class` comparison rather than `rt_task()`, because a stop-class task would pass `rt_task()` but is not genuinely an RT task suitable for `find_lowest_rq()`. Per the earlier priority check, `rq->curr` is guaranteed to have higher or equal priority to `next_task`, so if `rq->curr` is DL or stop-class, we simply bail out without needing to reschedule (there's no scheduling inversion).

**Third**, the SCHED_DEADLINE path in `push_dl_task()` (kernel/sched/deadline.c) receives a parallel fix: the `is_migration_disabled()` and `WARN_ON(next_task == rq->curr)` checks are moved after the priority/deadline comparison that calls `resched_curr()`. This ensures that if `rq->curr` is not a DL task or has a later deadline than `next_task`, the function reschedules rather than proceeding to the migration-disabled handling that could encounter stale state. As Dietmar Eggemann noted in review, the DL fix only compares against `dl_task(rq->curr)`, so it may miss rescheduling when `rq->curr` is a lower-priority non-DL task — but the DL path doesn't call `find_lowest_rq()` equivalent on `rq->curr` in the same way, so this is less critical.

## Triggering Conditions

The following conditions must all be satisfied simultaneously:

- **CONFIG_PREEMPT_RT or CONFIG_PREEMPT**: The bug is most reliably triggered on PREEMPT_RT kernels where `migrate_disable()` is used extensively (replacing `preempt_disable()` for many spinlocks). PREEMPT kernels also work but the specific code path through `rt_mutex_setprio()` for rt_mutex release is RT-specific in some kernel versions. The `rto_push_irq_work` mechanism exists on all SMP kernels with `HAVE_RT_PUSH_IPI` enabled.

- **Multiple CPUs (≥ 2)**: At least two CPUs are required. CPU X hosts the RT-overloaded runqueue where the crash occurs. CPU Y (or any other CPU) runs `rto_push_irq_work_func()` and sends the IPI to CPU X to trigger the push. The RTO IPI mechanism (`HAVE_RT_PUSH_IPI`) iterates through CPUs in the root domain's `rto_mask`.

- **RT overload on CPU X**: CPU X must have `rq->rt.overloaded == true`, meaning at least two RT tasks are runnable on it. One of these tasks must have migration disabled (on PREEMPT_RT, this commonly happens with ksoftirqd threads or any kernel thread holding a spinlock converted to an rt_mutex). The other is the currently running task `T_curr`, which is an originally-CFS task temporarily boosted to RT priority via PI.

- **rt_mutex contention**: A CFS task must acquire an rt_mutex that is also contended by an RT task, causing the CFS task to be PI-boosted to RT priority. When the CFS task later releases the mutex, `rt_mutex_setprio()` demotes it back to CFS. Common real-world scenarios include: console output through serial port (the serial8250 driver uses spinlocks that become rt_mutexes on PREEMPT_RT), udev startup, or any path with high lock contention.

- **Migration-disabled RT task on CPU X's pushable list**: There must be at least one RT task on CPU X that is (a) runnable but not currently executing, (b) has `nr_cpus_allowed > 1`, and (c) has `migration_disabled > 0`. This task will be selected by `pick_next_pushable_task()`. On PREEMPT_RT, ksoftirqd threads frequently enter `migrate_disable()` sections.

- **CPU X remains in `rto_mask` after demotion**: After `T_curr` is demoted from RT to CFS, CPU X must still be in the root domain's `rto_mask` (set when `rt_nr_migratory >= 2` via `rt_set_overload()`). If the demotion reduces the RT task count but other RT tasks remain migratory, the CPU stays in `rto_mask`. This allows another CPU's `rto_push_irq_work_func()` to select CPU X for the next push iteration.

- **Timing: IPI arrives before reschedule**: The `rto_push_work` irq_work must be delivered to CPU X in the narrow window after `raw_spin_rq_unlock(rq)` in `rt_mutex_setprio()` but before CPU X processes the pending reschedule (`TIF_NEED_RESCHED`). This window includes the time between the lock release and the next `preempt_schedule_irq()` call. An IPI arriving during this window will preempt the current code path and execute `rto_push_irq_work_func()` before the reschedule occurs.

The bug is probabilistic. The race window is typically microseconds wide. It was observed on real hardware during boot-time initialization when there is heavy rt_mutex contention (serial console output + udev + systemd startup all contending on converted spinlocks under PREEMPT_RT).

## Reproduce Strategy (kSTEP)

Reproducing this bug in kSTEP requires setting up the precise state where `push_rt_task()` encounters a non-RT `rq->curr` while processing a migration-disabled RT task. The strategy involves creating the conditions for the race and then rapidly iterating to hit the narrow timing window.

### CPU Configuration

Configure QEMU with at least 3 CPUs. CPU 0 is reserved for the kSTEP driver. CPU 1 will be the target CPU where the race occurs. CPU 2 serves as an additional CPU that can host the RTO IPI iteration and provide a valid target for `find_lowest_rq()`.

### Task and Thread Setup

1. **Create kthread `T_migdis` (migration-disabled RT task)**: Use `kstep_kthread_create("migdis")` to create a kernel thread. Bind it to CPUs 1 and 2 using `kstep_kthread_bind(T_migdis, cpumask_of(1) | cpumask_of(2))` so it has `nr_cpus_allowed > 1` (required to be on the pushable list). Set it to SCHED_FIFO using `sched_setscheduler_nocheck()` (imported via `KSYM_IMPORT`). The kthread's function should call `migrate_disable()`, then block via a completion or semaphore that the driver controls. This keeps the task on CPU 1's pushable list with migration disabled.

2. **Create kthread `T_curr` (the task to be demoted)**: Use `kstep_kthread_create("curr_rt")`. This thread will be temporarily boosted to RT and then demoted. Initially set it as SCHED_NORMAL (CFS). Pin it to CPU 1 initially.

3. **Create another RT task `T_high`**: Use `kstep_kthread_create("high_rt")`, set to SCHED_FIFO at a high priority. This ensures CPU 1 is RT-overloaded when both `T_high` and `T_migdis` are runnable.

### Triggering the Race

The most direct approach uses `KSYM_IMPORT(rt_mutex_setprio)` to explicitly invoke the deboost code path:

1. Start `T_migdis` on CPU 1 and have it call `migrate_disable()` then enter a blocking wait. It appears on CPU 1's RT pushable list with migration disabled.

2. Start `T_high` on CPU 1 as the highest-priority RT task. CPU 1 is now RT-overloaded.

3. Temporarily boost `T_curr` to RT priority using `rt_mutex_setprio()` (imported via `KSYM_IMPORT`). Have `T_curr` become the running task on CPU 1 at a priority between `T_high` and `T_migdis`.

4. In a tight loop on the driver (CPU 0), call `rt_mutex_setprio(T_curr, ...)` to demote `T_curr` back to CFS. Immediately after the demotion, the kernel will set `TIF_NEED_RESCHED` but the reschedule may not occur instantly.

5. Meanwhile, ensure that RTO push IPIs are active. Having RT overload on CPU 1 and the `rto_mask` set ensures that `rto_push_irq_work_func()` will eventually queue work on CPU 1.

### Alternative Approach Using sched_setscheduler

If direct `rt_mutex_setprio()` manipulation is too complex, an alternative approach:

1. Use `KSYM_IMPORT(__sched_setscheduler)` or `sched_setscheduler_nocheck()` to rapidly toggle `T_curr` between SCHED_FIFO and SCHED_NORMAL from the driver on CPU 0.

2. Each class change triggers `check_class_changed()` → `switched_from_rt()`, which mirrors the deboost path. While the exact internal state transitions differ slightly from the rt_mutex path, the observable effect is the same: `rq->curr` on CPU 1 transitions from RT to CFS while the RT overload state persists.

3. Run `kstep_tick_repeat(100000)` to generate many ticks, increasing the probability that `rto_push_irq_work_func()` fires on CPU 1 during the vulnerable window.

### Detection Criteria

Monitor for the bug using multiple methods:

1. **Kernel log checking**: After the test run, check `dmesg` output (captured in kSTEP logs) for:
   - `kernel BUG at kernel/sched/cpupri.c` — the crash itself
   - `WARNING: CPU: ... at kernel/sched/rt.c` — the WARN_ON in push_rt_task

2. **Instrumentation with `on_tick_begin` callback**: In the tick callback, check `rq->curr->sched_class` on CPU 1. If `rq->curr->sched_class == &fair_sched_class` and `rq->rt.overloaded == true`, log this anomalous state. If `find_lowest_rq` is subsequently called on this task, the bug is about to trigger.

3. **Pass/fail reporting**: Use `kstep_pass()` if the test completes without any BUG or WARNING in kernel logs. Use `kstep_fail()` if the crash or warning is detected. On the buggy kernel, the BUG/WARNING should eventually appear after sufficient iterations. On the fixed kernel, the early priority check and class check prevent the crash entirely.

### kSTEP Framework Considerations

The driver needs the following capabilities beyond the standard kSTEP API:

- **`KSYM_IMPORT(rt_mutex_setprio)` or `KSYM_IMPORT(sched_setscheduler_nocheck)`**: To change a task's scheduling class from another CPU's context. `KSYM_IMPORT` is already available in kSTEP's `internal.h`.

- **Custom kthread functions**: The `T_migdis` kthread needs a custom thread function that calls `migrate_disable()` and then blocks. kSTEP's `kstep_kthread_create()` allows custom kthreads, but the thread function may need to be defined in the driver. If the standard kthread actions don't support `migrate_disable()`, a small extension to `kmod/kthread.c` adding a `migrate_disable/enable` toggle action would be beneficial.

- **Access to `struct rq` internals**: Use `cpu_rq(1)->rt.overloaded`, `cpu_rq(1)->curr->sched_class`, etc., via kSTEP's `internal.h` includes for observation and assertion.

### Expected Behavior

- **Buggy kernel (v5.15–v5.17)**: After sufficient iterations (potentially thousands to tens of thousands of ticks), the kernel should hit `BUG()` in `convert_prio()` or `WARN_ON` in `push_rt_task()`. The exact iteration count depends on IPI timing. The driver reports `kstep_fail("BUG in convert_prio: push_rt_task called find_lowest_rq on non-RT curr")`.

- **Fixed kernel (v5.18+)**: The priority check at the top of the `retry:` loop catches the case where `rq->curr` has lower priority than `next_task` (always true when curr is CFS and next_task is RT). `resched_curr()` is called and the function returns safely. The `sched_class` check provides additional defense. No crash or warning occurs. After all iterations complete, the driver reports `kstep_pass("No BUG/WARNING detected in push_rt_task")`.

### Reliability Notes

This is a timing-dependent race. The race window (between `raw_spin_rq_unlock` in `rt_mutex_setprio` and the reschedule) is narrow. To maximize reproducibility:

1. Run with CONFIG_PREEMPT or CONFIG_PREEMPT_RT, which ensures `double_lock_balance()` always releases the source lock (increasing RTO push frequency).
2. Create high RT overload on multiple CPUs to keep the RTO IPI chain active.
3. Use rapid class-change toggling (FIFO ↔ NORMAL) to maximize the time spent in the vulnerable state.
4. Consider adding a small `udelay()` via a kernel patch (for testing only) after `raw_spin_rq_unlock` in `rt_mutex_setprio` to widen the race window.
5. If the race proves too hard to hit naturally, instrument `push_rt_task()` via `KSYM_IMPORT` to detect the precondition (non-RT curr + migration-disabled next_task) and report it even without the actual crash.
