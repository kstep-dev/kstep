# sched/core: Fix ttwu() race

- **Commit:** b6e13e85829f032411b896bd2f0d6cbe4b0a3c4a
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The kernel crashes with a NULL pointer dereference in `is_same_group()` when called from `sched_ttwu_pending()` during task wakeup. The crash occurs because `try_to_wake_up()` loads an stale `task_cpu(p)` value before observing that `p->on_cpu` is still set, leading to enqueuing the task on the wrong runqueue. This causes `p->se.cfs_rq` to point to a stale runqueue structure, resulting in a NULL deref when traversing the scheduling entity hierarchy.

## Root Cause

A race condition exists where `task_cpu(p)` is loaded early in `try_to_wake_up()` before checking `p->on_cpu`. If the task migrates between CPUs while `p->on_cpu` is still set (task is still running on the old CPU), the stale CPU value causes the task to be enqueued on the wrong runqueue. The memory ordering between these two loads is incorrect, allowing `task_cpu(p)` to return a value from before the task actually migrated, even though `p->on_cpu` correctly indicates the task is still running.

## Fix Summary

The fix reorders the memory loads in `try_to_wake_up()` to read `p->on_cpu` before `task_cpu(p)`, using `smp_load_acquire()` for proper synchronization. Additionally, defensive checks are added in `sched_ttwu_pending()` to catch inconsistent task state and correct it, and a guard is added in `ttwu_queue_wakelist()` to prevent enqueuing to the current CPU.

## Triggering Conditions

The race requires a precise sequence where a task is migrating between CPUs while try_to_wake_up() is called. The key conditions are:
- Task X running on CPU1, then switches away and migrates to CPU0 
- Task X starts running on CPU0, then goes to sleep (X->state = TASK_UNINTERRUPTIBLE)
- Waker calls ttwu() on another CPU while X is calling schedule() on CPU0
- ttwu() loads stale task_cpu(X) value (e.g., CPU1) before loading p->on_cpu
- X finishes context switch (X->on_cpu = 0) after ttwu() reads p->on_cpu = 1
- This causes ttwu() to enqueue X on wrong CPU, leading to mismatched p->se.cfs_rq pointers
- Later sched_ttwu_pending() → check_preempt_curr() → is_same_group() dereferences NULL cfs_rq

## Reproduce Strategy (kSTEP)

Need minimum 3 CPUs (CPU 0 reserved). Create task X that migrates from CPU1 to CPU2:
- Use `kstep_task_create()` to create task X and waker task
- Pin X to CPU1 initially with `kstep_task_pin(x, 1, 1)`, let it run several ticks
- Use `kstep_task_pin(x, 2, 2)` to trigger migration from CPU1 to CPU2 
- Once X is running on CPU2, use `kstep_task_pause(x)` to put X to sleep
- Immediately call `kstep_task_wakeup(x)` from waker to trigger race window
- Use `on_tick_begin()` callback to log task_cpu(X), X->on_cpu, and X->se.cfs_rq values
- Monitor for WARN_ON_ONCE() messages in sched_ttwu_pending() indicating stale CPU detected
- Trigger check_preempt_curr() path via competing tasks to expose NULL deref in is_same_group()
