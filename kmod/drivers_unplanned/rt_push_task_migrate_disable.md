# RT: get_push_task() Missing migrate_disable() Check

**Commit:** `e681dcbaa4b284454fecd09617f8b24231448446`
**Affected files:** kernel/sched/sched.h
**Fixed in:** v5.14
**Buggy since:** v5.11-rc1 (introduced by commit a7c81556ec4d3 "sched: Fix migrate_disable() vs rt/dl balancing")

## Bug Description

The Linux kernel's RT and DL (deadline) schedulers include a push/pull balancing mechanism that attempts to migrate tasks between CPUs to ensure the highest-priority tasks can run. When a CPU discovers that the next runnable RT or DL task has migration disabled (via `migrate_disable()`), the scheduler tries to push the *currently running* task to another CPU instead, thereby freeing the local CPU for the pinned higher-priority task. This push operation is orchestrated through `get_push_task()`, which identifies the current task as a candidate for migration, and then `push_cpu_stop()`, which is invoked via the stopper thread to actually move the task.

The `get_push_task()` function in `kernel/sched/sched.h` was introduced by commit a7c81556ec4d3 to support this mechanism. It checks whether the current task (`rq->curr`) can be migrated by verifying that the runqueue is not already push-busy (`rq->push_busy`) and that the task's `nr_cpus_allowed` is not 1 (meaning it is not hard-pinned to a single CPU). However, the function neglects to check the `migration_disabled` field of the task. A task with `migration_disabled > 0` is effectively pinned to its current CPU even though its `nr_cpus_allowed` may be greater than 1 — the `migrate_disable()` mechanism temporarily overrides the task's cpumask via `p->cpus_ptr` without modifying `nr_cpus_allowed`.

As a result, when both the currently running task and the next pushable task both have migration disabled, `get_push_task()` incorrectly selects the current task for pushing. This triggers a `stop_one_cpu_nowait()` call to invoke `push_cpu_stop()` on the migration/stopper thread, which then discovers the task cannot actually be moved (because `is_migration_disabled(p)` is true) and aborts. The net effect is a pointless and potentially disruptive invocation of the CPU stopper mechanism.

## Root Cause

The root cause is an incomplete predicate in `get_push_task()`. The function is defined in `kernel/sched/sched.h` and is called from three distinct code paths:

1. **`push_rt_task()` in `kernel/sched/rt.c`**: When the next pushable RT task has migration disabled, the function calls `get_push_task(rq)` to get the currently running task as a candidate for pushing to a lower-priority CPU.

2. **`pull_rt_task()` in `kernel/sched/rt.c`**: When pulling from a source runqueue and the candidate task has migration disabled, `get_push_task(src_rq)` is called to push the source CPU's current task away instead.

3. **`pull_dl_task()` in `kernel/sched/deadline.c`**: Similarly, when the earliest pushable DL task has migration disabled, `get_push_task(src_rq)` is called.

The buggy `get_push_task()` code is:

```c
static inline struct task_struct *get_push_task(struct rq *rq)
{
    struct task_struct *p = rq->curr;

    lockdep_assert_rq_held(rq);

    if (rq->push_busy)
        return NULL;

    if (p->nr_cpus_allowed == 1)
        return NULL;

    /* BUG: missing check for p->migration_disabled */

    rq->push_busy = true;
    return get_task_struct(p);
}
```

The check `p->nr_cpus_allowed == 1` only catches tasks that are permanently affine to a single CPU (e.g., via `sched_setaffinity()`). It does not catch tasks that are temporarily pinned via `migrate_disable()`. When a task calls `migrate_disable()`, its `migration_disabled` counter is incremented and its `cpus_ptr` is set to a single-CPU mask, but `nr_cpus_allowed` (which reflects the original affinity mask weight) remains unchanged. This means `get_push_task()` sees `nr_cpus_allowed > 1` and proceeds to select the task for migration.

The stopper thread's `push_cpu_stop()` function correctly checks `is_migration_disabled(p)` and handles it by setting `p->migration_flags |= MDF_PUSH` (to defer the push until `migrate_enable()`). However, the entire stopper thread invocation is unnecessary overhead when `get_push_task()` could have detected the situation and returned NULL immediately.

The scenario that triggers this bug requires two RT (or DL) tasks on the same CPU, both with `migrate_disable()` active. The lower-priority task is on the pushable list, and the higher-priority task (current) also has migration disabled. When the scheduler attempts push balancing, it finds the pushable task is migration-disabled, then tries to push the current task instead, but `get_push_task()` fails to notice that the current task is also migration-disabled.

## Consequence

The observable consequence is a **pointless invocation of the CPU stopper mechanism** (the migration/stopper kthread). While this does not cause a crash, hang, or data corruption, it has several negative impacts:

1. **Unnecessary CPU overhead**: The `stop_one_cpu_nowait()` call queues work on the stopper thread, which then runs `push_cpu_stop()`. This function acquires `p->pi_lock` and `rq->lock`, checks the migration_disabled state, sets the MDF_PUSH flag, and bails out. All of this work is wasted — the same information was available to the caller in `get_push_task()`.

2. **Stopper thread latency impact**: The stopper thread runs at the highest priority (SCHED_FIFO, MAX_RT_PRIO - 1). Every unnecessary invocation delays any other pending stopper work and adds latency to the system. On PREEMPT_RT systems where `migrate_disable()` is used extensively (replacing preempt_disable for spinlock-held regions), this spurious activity could become frequent.

3. **Push_busy blocking**: When `get_push_task()` sets `rq->push_busy = true` and returns the task, no other push operation can proceed on that runqueue until the stopper thread completes and clears `push_busy`. This means the runqueue is locked out of push operations for the entire duration of the unnecessary stopper invocation, potentially delaying legitimate push operations.

The bug is specific to `CONFIG_PREEMPT_RT` kernels, as `migration_disabled` is only present when `CONFIG_SMP && CONFIG_PREEMPT_RT` are both enabled. On non-PREEMPT_RT kernels, `is_migration_disabled()` always returns false and `migration_disabled` does not exist in the task structure.

## Fix Summary

The fix adds a single check to `get_push_task()` in `kernel/sched/sched.h`:

```c
if (p->migration_disabled)
    return NULL;
```

This check is inserted after the existing `nr_cpus_allowed == 1` check and before the `rq->push_busy = true` assignment. With this fix, `get_push_task()` returns NULL for any task that has migration disabled, preventing the unnecessary invocation of the stopper thread.

The fix is minimal and correct: it ensures that `get_push_task()` only returns tasks that can actually be migrated. The function already had the intent of filtering unmovable tasks (via the `nr_cpus_allowed` check), and the `migration_disabled` check completes that intent. By returning NULL early, the callers (`push_rt_task()`, `pull_rt_task()`, `pull_dl_task()`) simply see that no push is possible and move on, which is the correct behavior — if neither the pushable task nor the current task can move, no push operation should be attempted.

As noted in the LKML review by Tao Zhou, the `pull_rt_task()` caller already checks `is_migration_disabled(p)` on the pushable task *before* calling `get_push_task()`, so the check appears redundant in that call path. However, `get_push_task()` checks the *current* task (`rq->curr`), not the pushable task, so the checks are on different tasks and are not actually redundant. The fix correctly handles the case where the current task (the one being considered for push-away) itself has migration disabled.

## Triggering Conditions

The following conditions must all be met to trigger the bug:

- **CONFIG_PREEMPT_RT must be enabled**: The `migration_disabled` field only exists under `CONFIG_SMP && CONFIG_PREEMPT_RT`. On non-PREEMPT_RT kernels, `migrate_disable()` maps to `preempt_disable()` and there is no separate `migration_disabled` tracking.

- **At least 2 CPUs**: SMP is required for RT/DL push/pull balancing to be relevant.

- **Two or more RT (or DL) tasks on the same CPU**: The runqueue must be overloaded (`rq->rt.overloaded` or `rq->dl.overloaded`), meaning there is at least one pushable task beyond the currently running one.

- **The next pushable task has `migrate_disable()` active**: This causes `push_rt_task()` to enter the "push current instead" code path rather than migrating the pushable task directly.

- **The currently running task also has `migrate_disable()` active but `nr_cpus_allowed > 1`**: This is the key condition. The current task must not be hard-pinned (so `nr_cpus_allowed > 1`), but must have `migration_disabled > 0`.

- **A lower-priority CPU must be available** (for the RT path): `find_lowest_rq(rq->curr)` must return a valid CPU, otherwise `push_rt_task()` returns before calling `get_push_task()`.

- **The bug triggers during pull operations**: Pull balancing (`pull_rt_task()`, `rto_push_irq_work_func()`, or `pull_dl_task()`) passes `pull=true` to `push_rt_task()`, which enables the "push current away" path.

On a PREEMPT_RT system with heavy spinlock contention (where spinlocks become sleeping locks with `migrate_disable()`), having multiple RT tasks in migrate_disable sections simultaneously is plausible and could trigger this bug frequently.

## Reproduce Strategy (kSTEP)

### Why this bug cannot be reproduced with kSTEP

1. **Kernel version too old**: The bug was introduced in v5.11-rc1 by commit a7c81556ec4d3 and fixed in v5.14 by commit e681dcbaa4b284454fecd09617f8b24231448446. kSTEP supports Linux v5.15 and newer only. Since the fix landed in v5.14, all v5.15+ kernels already contain the fix, making the bug unreproducible with kSTEP.

2. **CONFIG_PREEMPT_RT requirement**: Even if the kernel version were compatible, this bug requires `CONFIG_PREEMPT_RT` to be enabled. The `migration_disabled` field in `task_struct` and the `migrate_disable()`/`migrate_enable()` functions that manipulate it are conditionally compiled under `#if defined(CONFIG_SMP) && defined(CONFIG_PREEMPT_RT)`. Without PREEMPT_RT, `is_migration_disabled()` is a static inline returning `false`, and the buggy code path in `get_push_task()` is unreachable. kSTEP typically builds non-PREEMPT_RT kernels.

3. **migrate_disable() API unavailability in kSTEP**: kSTEP does not expose a `migrate_disable()` / `migrate_enable()` API for tasks. While kSTEP can create RT tasks (`kstep_task_fifo(p)`), pin them to CPUs (`kstep_task_pin(p, begin, end)`), and manage their lifecycle, it cannot put a task into the `migration_disabled` state. The `migrate_disable()` call is a per-task operation that must be executed in the task's own context (it modifies `current->migration_disabled`), not from an external driver.

### What would need to change to support this

To reproduce this bug, the following would be needed:

- **kSTEP would need to support a pre-v5.15 kernel** (specifically v5.11 through v5.13), or the fix would need to have been reverted on a v5.15+ kernel for testing purposes.

- **PREEMPT_RT kernel configuration**: kSTEP would need to build and boot a `CONFIG_PREEMPT_RT` kernel.

- **A `kstep_task_migrate_disable(p)` / `kstep_task_migrate_enable(p)` API**: This would allow the driver to put tasks into the migration-disabled state. Implementing this would require executing `migrate_disable()` in the task's context, possibly via a tasklet or by having the kthread execute a callback.

### Alternative reproduction methods

Outside kSTEP, this bug could be reproduced on a v5.11-v5.13 PREEMPT_RT kernel by:

1. Creating two SCHED_FIFO tasks with different priorities, both pinned to the same CPU via affinity (but with `nr_cpus_allowed > 1` on at least a 2-CPU subset).
2. Having both tasks execute code that calls `migrate_disable()` (e.g., acquiring an rt_mutex/spinlock in PREEMPT_RT).
3. Running a third lower-priority task on another CPU that triggers `pull_rt_task()` (e.g., by the RT overload IPI mechanism).
4. Observing the unnecessary stopper thread activation via tracing (`trace-cmd` / `ftrace` with the `sched_switch` and `stop_machine` tracepoints).
