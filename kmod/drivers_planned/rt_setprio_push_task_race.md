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

Reproducing this bug in kSTEP requires setting up the precise state where `push_rt_task()` encounters a non-RT `rq->curr` while processing a migration-disabled RT task. The strategy uses exclusively public kernel APIs and kSTEP interfaces to reach the vulnerable state — all scheduler-internal fields are accessed read-only for observation and assertion purposes only. The key insight is that the original bug is triggered by natural rt_mutex PI boost/deboost and RTO push IPIs, both of which can be provoked entirely through standard kernel locking and scheduling APIs without directly writing to any internal scheduler structures.

### CPU Configuration

Configure QEMU with at least 3 CPUs (`-smp 3`). CPU 0 is reserved for the kSTEP driver. CPU 1 is the target CPU where the race will be triggered — it must host the RT-overloaded runqueue with a migration-disabled pushable RT task and the task undergoing demotion from RT to CFS. CPU 2 (and optionally additional CPUs) serves as an additional CPU in the root domain, providing a valid target for `find_lowest_rq()` and hosting CPUs that run `rto_push_irq_work_func()` to send the critical IPI to CPU 1. Boot with `isolcpus=` left unset so all CPUs participate in the root domain's `rto_mask`. Build with `CONFIG_PREEMPT_RT=y` (preferred, since `migrate_disable()` has its full per-task implementation and spinlocks become rt_mutexes) or at minimum `CONFIG_PREEMPT=y`. Ensure `CONFIG_SMP=y` and `CONFIG_HAVE_RT_PUSH_IPI=y` so the RTO IPI push mechanism is active.

### Migration-Disabled RT Task Setup

The bug requires a migration-disabled RT task on CPU 1's pushable list. Since the kSTEP kthread framework's built-in actions (`do_spin`, `do_yield`, `do_block_on_wq`) do not call `migrate_disable()`, the driver creates a dedicated kernel thread using `kthread_create()` — a standard kernel API available to all GPL modules — with a custom function body. This custom function calls `migrate_disable()` (exported via `EXPORT_SYMBOL_GPL`, a public kernel API for disabling task migration), then waits on a `struct completion` controlled by the driver, effectively parking the thread in a migration-disabled state while remaining on the runqueue. Before starting the thread, use `kstep_task_fifo(T_migdis)` to set it to SCHED_FIFO (this internally calls `sched_setattr_nocheck()`, the proper kernel API for scheduling attribute changes). Use `kstep_kthread_bind(T_migdis, mask)` with a mask covering CPUs {1, 2} so that `nr_cpus_allowed > 1`, which is required for the task to appear on the pushable task list via `enqueue_pushable_task()`. Once woken on CPU 1, `T_migdis` will be enqueued as an RT task with its per-task `migration_disabled` counter incremented — exactly the condition that steers `push_rt_task()` into the `is_migration_disabled(next_task)` branch.

Alternatively, to stay entirely within the kSTEP kthread framework, extend `kmod/kthread.c` with a new action callback `do_migrate_disable_spin` that calls `migrate_disable()` on its first invocation (guarded by a flag) and then busy-spins. The driver can then use `kstep_kthread_create("migdis")`, bind, set to FIFO, start, and switch the thread to this new action. Either approach modifies the task's migration state through the proper kernel API (`migrate_disable()`) rather than by writing to `p->migration_disabled` or any other internal field directly.

### Primary Approach: Natural rt_mutex PI Boost and Deboost

The most faithful reproduction uses actual rt_mutex priority inheritance to trigger the exact code path described in the commit message: `mark_wakeup_next_waiter()` → `rt_mutex_adjust_prio()` → `rt_mutex_setprio()` → `check_class_changed()` → `switched_from_rt()` / `switched_to_fair()`. The driver declares a `struct rt_mutex test_mutex` and initializes it with `rt_mutex_init(&test_mutex)` — both are standard kernel locking primitives exported to modules. Two additional kernel threads are created with `kthread_create()`:

- **T_holder** (initially CFS, pinned to CPU 1): Its custom function body calls `rt_mutex_lock(&test_mutex)`, then signals a `struct completion holder_ready` to notify the driver it holds the lock, then waits on `struct completion holder_release` for the driver's release signal. When signaled, it calls `rt_mutex_unlock(&test_mutex)`, which triggers the natural deboost path.
- **T_contender** (SCHED_FIFO via `kstep_task_fifo()`, pinned to CPU 1 or allowed to migrate): Its function body calls `rt_mutex_lock(&test_mutex)`, which blocks because T_holder already holds the lock. The kernel's PI mechanism automatically boosts T_holder to T_contender's RT priority through the standard rt_mutex PI chain (`rt_mutex_adjust_prio()` → `rt_mutex_setprio()`).

The race sequence proceeds as follows: (1) T_holder starts on CPU 1 and acquires `test_mutex`. The driver waits on `holder_ready` to confirm. (2) T_contender starts as SCHED_FIFO and blocks on `test_mutex`, causing the kernel to PI-boost T_holder to T_contender's RT priority. Now CPU 1 has T_holder running as a PI-boosted RT task and T_migdis queued as a pushable RT task with migration disabled — the CPU is RT-overloaded (`rq->rt.overloaded == true`) and in the `rto_mask`. (3) The driver signals `holder_release`, causing T_holder to call `rt_mutex_unlock(&test_mutex)`. Inside `rt_mutex_slowunlock()`, the kernel calls `mark_wakeup_next_waiter()` → `rt_mutex_adjust_prio()` → `rt_mutex_setprio()`, which demotes T_holder back to CFS (SCHED_NORMAL, priority 120). `check_class_changed()` invokes `switched_from_rt()` (which does NOT remove CPU 1 from `rto_mask` because `T_migdis` keeps `rt_nr_running > 0`) and `switched_to_fair()` (which sets `TIF_NEED_RESCHED`). (4) After `raw_spin_rq_unlock(rq)`, there is a narrow window before CPU 1 processes the pending reschedule. If an `rto_push_irq_work` IPI arrives from another CPU during this window, `rto_push_irq_work_func()` runs, checks `has_pushable_tasks(rq)` (true, T_migdis is pushable), and calls `push_rt_task(rq, true)`. (5) `pick_next_pushable_task()` returns T_migdis. `is_migration_disabled(T_migdis)` is true. In the buggy kernel, `find_lowest_rq(rq->curr)` is called on the now-CFS T_holder (priority 120), crashing in `convert_prio()`. All operations in this sequence use exclusively public kernel APIs (`rt_mutex_lock`, `rt_mutex_unlock`, `kstep_task_fifo`, `complete`, `wait_for_completion`). No internal scheduler fields are written by the driver.

### Alternative Approach: Rapid Scheduling-Class Toggling via kSTEP API

If the rt_mutex contention approach proves too complex to orchestrate or the race window is too narrow to hit reliably, a simpler alternative achieves the same vulnerable state by toggling a task's scheduling class rapidly using kSTEP's own API. Create kthread `T_target` using `kstep_kthread_create("target")`, bind it to CPU 1 via `kstep_kthread_bind()`, start it with `kstep_kthread_start()`, and set it to SCHED_FIFO via `kstep_task_fifo(T_target)`. With `T_migdis` also on CPU 1 as a pushable migration-disabled RT task, CPU 1 is RT-overloaded. From CPU 0, in a tight loop, alternate between `kstep_task_cfs(T_target)` (demoting to CFS) and `kstep_task_fifo(T_target)` (promoting back to RT). Each call to `kstep_task_cfs()` internally invokes `sched_setattr_nocheck()` → `__sched_setscheduler()` → `__setscheduler_prio()` → `check_class_changed()` → `switched_from_rt()` + `switched_to_fair()`, which sets `TIF_NEED_RESCHED` and opens the same vulnerable window between `raw_spin_rq_unlock(rq)` and the actual reschedule. While the exact lock acquisition/release sequence differs slightly from the rt_mutex PI path, the net effect is identical: `rq->curr` on CPU 1 transitions from `rt_sched_class` to `fair_sched_class` while the CPU remains in the `rto_mask` with a migration-disabled pushable RT task. Interleave `kstep_tick_repeat(1000)` between toggling bursts to generate scheduling activity and increase the probability that `rto_push_irq_work_func()` fires on CPU 1 during a demotion window.

### Increasing RTO IPI Frequency

To maximize the probability that an RTO push IPI hits CPU 1 in the narrow demotion window, create RT overload across multiple CPUs in the root domain. Use `kstep_kthread_create()` and `kstep_task_fifo()` to place two or more SCHED_FIFO tasks on each of CPUs 2, 3, etc. (if available), ensuring each CPU has `rq->rt.overloaded == true` and is in the `rto_mask`. The RTO IPI chain (driven by `rto_push_irq_work_func()`) iterates through CPUs in the root domain's `rto_mask` using `rto_next_cpu()`. With more CPUs overloaded, the chain visits CPU 1 more frequently, increasing the rate of `push_rt_task()` invocations on CPU 1. Additionally, creating pushable RT tasks on CPU 2 that can be migrated ensures `find_lowest_rq()` has valid targets, which is necessary for the `is_migration_disabled()` branch to proceed past the `cpu == -1` check.

### Orchestration and Timing

Use the `on_tick_begin` callback to coordinate and monitor the race from CPU 0. On each tick, the callback reads (read-only) `cpu_rq(1)->rt.overloaded` and `cpu_rq(1)->curr->sched_class` to verify that the preconditions are in place. When the overloaded state is confirmed, the driver triggers the next class-toggle iteration or signals the rt_mutex holder to release. For the rt_mutex approach, the full PI boost/deboost cycle must be re-established on each iteration: after T_holder releases and gets descheduled, the driver must re-create the contention by having T_holder re-acquire the mutex and T_contender (or a new contender) block on it again. Use `struct completion` pairs (one for "lock acquired" and one for "release now") to synchronize each cycle without busy-waiting or polling. For the class-toggling approach, the driver simply alternates `kstep_task_fifo()` / `kstep_task_cfs()` calls as fast as possible from the driver's main loop, interleaving ticks to allow the scheduler to process the resulting state changes.

The probabilistic nature of the race requires many iterations. Run the test for tens of thousands of ticks, performing the toggle/release operation on each tick where preconditions are satisfied. The race window (between `raw_spin_rq_unlock` and `preempt_schedule_irq`) is typically microseconds wide, so high iteration counts are essential for reliable reproduction.

### Detection Criteria

Monitor for the bug using read-only observation and kernel log output:

1. **Kernel log checking**: After the test run, check `dmesg` output (captured in kSTEP logs) for `kernel BUG at kernel/sched/cpupri.c` (the crash in `convert_prio()`) or `WARNING: CPU: ... at kernel/sched/rt.c` (the `WARN_ON(next_task == rq->curr)` check). Either message confirms the bug was triggered.

2. **Read-only observation via `on_tick_begin`**: On each tick, read `cpu_rq(1)->curr->sched_class` and `cpu_rq(1)->rt.overloaded` (both read-only accesses via kSTEP's `internal.h` includes). If `rq->curr->sched_class == &fair_sched_class` while `rq->rt.overloaded == true`, log this anomalous state — it confirms the precondition for the bug was reached. Track how many times this precondition is observed to gauge test effectiveness. Optionally, also read `cpu_rq(1)->rt.rt_nr_running` and inspect `cpu_rq(1)->rt.pushable_tasks` to confirm T_migdis is on the pushable list.

3. **Pass/fail reporting**: Use `kstep_fail("BUG in convert_prio: push_rt_task called find_lowest_rq on non-RT curr")` if the crash or warning is detected in kernel logs. Use `kstep_pass("No BUG/WARNING detected in push_rt_task after N iterations")` if all iterations complete without the crash. On the buggy kernel (v5.11–v5.17), the BUG/WARNING should appear after sufficient iterations. On the fixed kernel (v5.18+), the moved priority check (`next_task->prio < rq->curr->prio`) at the top of `retry:` catches the case immediately (any RT prio 0–98 < CFS prio 120), calling `resched_curr()` and returning. The additional `rq->curr->sched_class != &rt_sched_class` guard inside the `is_migration_disabled()` branch provides defense-in-depth.

### kSTEP Framework Considerations

The driver uses the following capabilities, all achieved through public kernel APIs or kSTEP's standard interfaces — no internal scheduler fields are written:

- **`kthread_create()` / `wake_up_process()`**: Standard kernel APIs for creating kthreads with custom function bodies (for T_migdis, T_holder, T_contender). Directly available to GPL kernel modules without `KSYM_IMPORT`.
- **`migrate_disable()` / `migrate_enable()`**: Public kernel API (`EXPORT_SYMBOL_GPL`), called from within T_migdis's kthread function body. This is the proper kernel interface for disabling task migration — it increments the per-task `migration_disabled` counter through the sanctioned code path, not by writing the field directly.
- **`rt_mutex_init()` / `rt_mutex_lock()` / `rt_mutex_unlock()`**: Standard kernel locking primitives, exported to modules. Used in the primary approach to create natural PI boost/deboost scenarios that trigger the exact `rt_mutex_setprio()` code path of the original bug — without calling `rt_mutex_setprio()` directly.
- **`struct completion` / `complete()` / `wait_for_completion()`**: Standard kernel synchronization primitives for coordinating kthread actions (lock-acquired, release-now) with the driver on CPU 0.
- **`kstep_task_fifo()` / `kstep_task_cfs()`**: kSTEP API calls that internally use `sched_setattr_nocheck()` — the proper kernel API for changing scheduling attributes. Used in the alternative approach for rapid class toggling and in both approaches for initial RT task setup.
- **`kstep_kthread_create()` / `kstep_kthread_bind()` / `kstep_kthread_start()`**: kSTEP kthread management APIs for auxiliary RT tasks that create overload on multiple CPUs.
- **`KSYM_IMPORT(set_cpus_allowed_ptr)`**: May be needed to set CPU affinity with an arbitrary multi-CPU mask (e.g., CPUs {1, 2}) if `kstep_kthread_bind()` is insufficient. `set_cpus_allowed_ptr()` is a public kernel API.
- **Read-only access to `struct rq` internals**: `cpu_rq(1)->rt.overloaded`, `cpu_rq(1)->curr->sched_class`, `cpu_rq(1)->rt.rt_nr_running`, etc., via kSTEP's `internal.h` includes — used exclusively for observation and assertion, never written.

### Expected Behavior

- **Buggy kernel (v5.11–v5.17)**: After sufficient iterations (potentially thousands to tens of thousands of cycles), the kernel hits `BUG()` in `convert_prio()` (on ARM/ARM64, manifesting as `kernel BUG at kernel/sched/cpupri.c:151!` with the CFS priority value 120) or `WARN_ON` in `push_rt_task()`. The crash occurs when `push_rt_task()` runs via `rto_push_irq_work_func()` IPI during the narrow window between `rq->curr` demotion from RT to CFS and the pending reschedule. The exact iteration count depends on IPI delivery timing and CPU load. The driver reports `kstep_fail("BUG in convert_prio: push_rt_task called find_lowest_rq on non-RT curr")`.

- **Fixed kernel (v5.18+)**: The moved priority check at the top of the `retry:` loop catches the case where `next_task->prio` (RT, 0–98) `< rq->curr->prio` (CFS, 120), calling `resched_curr()` and returning immediately without entering the `is_migration_disabled()` branch. The additional `rq->curr->sched_class != &rt_sched_class` guard inside the `is_migration_disabled()` branch provides defense-in-depth for cases where a stop-class task (masquerading as SCHED_FIFO with prio 0) is current. No crash or warning occurs. After all iterations complete, the driver reports `kstep_pass("No BUG/WARNING detected in push_rt_task after N iterations")`.

### Reliability Notes

This is a timing-dependent race with a microsecond-wide window. To maximize reproducibility: (1) Build with `CONFIG_PREEMPT_RT=y`, which ensures `migrate_disable()` has its full per-task implementation (not just `preempt_disable()`) and increases the frequency of `rto_push_irq_work` due to more frequent rt_mutex contention from converted spinlocks. (2) Create high RT overload on multiple CPUs to keep the RTO IPI chain active and frequently visiting CPU 1. (3) The rt_mutex approach triggers the exact original code path (`mark_wakeup_next_waiter` → `rt_mutex_setprio`) and is the most authentic reproduction, but requires careful kthread orchestration with completions. The class-toggling approach has a higher iteration rate (no need to re-establish PI contention each cycle) and compensates for lower per-iteration fidelity with volume. (4) If the race proves too hard to hit naturally after many iterations, use the read-only `on_tick_begin` observation to confirm the precondition (non-RT curr + RT overload + migration-disabled pushable task) was established, demonstrating that the vulnerable state was reached even if the IPI did not arrive in the exact window. This serves as a partial validation that the test setup is correct and the bug is latent.
