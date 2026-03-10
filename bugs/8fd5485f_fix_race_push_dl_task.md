# sched/deadline: Fix race in push_dl_task()

- **Commit:** 8fd5485fb4f3d9da3977fd783fcb8e5452463420
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

When `push_dl_task()` attempts to migrate a deadline task to another CPU via `find_lock_later_rq()`, a race condition exists where the task can be migrated and executed on a different CPU, then awakened and re-queued on the original CPU before the lock is reacquired. The existing validation checks would incorrectly pass in this scenario, allowing an invalid task migration attempt that violates the scheduler's invariants about which tasks are available for pushing.

## Root Cause

The `find_lock_later_rq()` function calls `double_lock_balance()` which releases the current runqueue's lock to avoid deadlock when acquiring both runqueue locks. During this window, the task can be migrated away, executed, and re-queued by another CPU. The original validation logic only checked generic task properties (whether task is on rq, is a dl task, etc.) but did not verify that the task was still at the head of the pushable deadline tasks list—the invariant required for push operations.

## Fix Summary

The fix introduces `pick_next_pushable_dl_task()` to extract the top task from the pushable deadline tasks list and adds a differentiated check: for non-throttled deadline tasks (push_dl_task), it verifies the task is still at the head of the pushable list after locks are obtained; for throttled tasks (dl_task_offline_migration), existing checks suffice. This ensures only valid tasks are migrated by preventing the race condition where a task becomes invalid between lock release and reacquisition.

## Triggering Conditions

The deadline scheduler's `push_dl_task()` race occurs when multiple CPUs compete for runqueue locks during task migration. Key conditions:
- Multiple deadline tasks with overlapping CPU affinity masks across at least 2 CPUs 
- One CPU calls `push_dl_task()` to migrate its highest-priority pushable deadline task
- During `find_lock_later_rq()`, `double_lock_balance()` temporarily releases the source runqueue lock
- In this window, another CPU migrates and executes the target task, then re-queues it via `ttwu()`
- The task appears valid (on_rq=1, dl_task, same CPU) but is no longer the pushable list head
- Original validation checks pass incorrectly, allowing invalid migration attempt

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved for driver). Create contention between deadline tasks across CPUs:
1. **Setup:** Use `kstep_topo_init()` and `kstep_topo_apply()` for basic SMP topology
2. **Task creation:** Create 3-4 deadline tasks with `kstep_task_create()` and set deadline scheduling policy 
3. **CPU affinity:** Pin tasks to overlapping CPU sets (e.g., tasks 1-2 on CPUs 1-3, task 3 on CPUs 2-4)
4. **Load generation:** Use `kstep_tick_repeat()` and `kstep_task_wakeup()` to create scheduling pressure
5. **Race trigger:** Force `push_dl_task()` calls by overloading specific CPUs with deadline tasks
6. **Detection:** Use `on_sched_balance_begin()` callback to log pushable task list state before/after lock operations
7. **Validation:** Check if a task remains at pushable list head after migration attempts using internal scheduler state inspection
