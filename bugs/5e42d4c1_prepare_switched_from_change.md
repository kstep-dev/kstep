# sched/deadline: Prepare for switched_from() change

- **Commit:** 5e42d4c123ba9b89ce19b3aa7e22b7684cbfa49c
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline (SCHED_DEADLINE)

## Bug Description

The kernel scheduler is restructuring the order of operations when switching task scheduling classes. Currently, `switched_from()` is called after the task's class is changed; however, it will be moved to execute before the class change. This creates a critical issue in the deadline scheduler: `switched_from_dl()` calls `task_non_contending()`, which checks `dl_task(p)` to determine task status. Since `dl_task()` relies on `p->prio` (which changes during class switching), calling this check before the class change is complete would incorrectly report the task as non-deadline when it is still a deadline task, leading to incorrect bandwidth accounting and missed deadline-specific cleanup.

## Root Cause

The bug stems from a timing dependency: `task_non_contending()` uses `dl_task(p)` to determine whether a task is still a deadline task, but this check depends on `p->prio` being current. When `switched_from_dl()` is called before the task class is changed (instead of after), `p->prio` will not yet reflect the new scheduling class, causing `dl_task()` to return false for a task that is still in the deadline class at that moment.

## Fix Summary

The fix introduces a boolean parameter `dl_task` to `task_non_contending()` to pass the task's deadline status explicitly rather than computing it via `dl_task(p)`. Callers pass `true` when the task is still a deadline task (dequeue path) and `false` when it is already transitioning out of the deadline class (switched_from path), ensuring correct behavior regardless of when the call occurs in the scheduling class switch sequence.

## Triggering Conditions

The bug occurs when a SCHED_DEADLINE task switches to a different scheduling class (CFS/RT), specifically during the execution of `switched_from_dl()`. Critical conditions include:
- A task currently using SCHED_DEADLINE policy that needs to switch classes
- The switch triggered by priority inheritance, sched_setscheduler(), or task termination  
- `task_non_contending()` called from `switched_from_dl()` path (not dequeue path)
- Task's `p->prio` already modified to reflect new scheduling class before `switched_from_dl()` execution
- The deadline scheduler's bandwidth accounting relies on correct identification of deadline tasks
- Timing-sensitive: occurs only when `switched_from()` is called before class change vs. after

## Reproduce Strategy (kSTEP)

Reproduce by forcing a deadline task to switch scheduling classes and observing incorrect bandwidth accounting:
- Use 2+ CPUs (CPU 0 reserved for driver)  
- In `setup()`: Create a SCHED_DEADLINE task with `kstep_task_create()` and configure deadline parameters via direct manipulation or cgroup interface
- In `run()`: Start task with `kstep_task_wakeup()`, let it run with `kstep_tick_repeat(10)`
- Force class switch using `kstep_task_set_prio()` or cgroup migration to trigger `switched_from_dl()`
- Use `on_tick_begin()` callback to log deadline bandwidth (`dl_rq->running_bw`, `dl_rq->this_bw`)
- Check that bandwidth accounting becomes inconsistent when `dl_task(p)` incorrectly returns false
- Alternative: Use priority inheritance by creating mutex contention between deadline and RT tasks
