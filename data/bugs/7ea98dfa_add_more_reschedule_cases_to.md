# sched/deadline: Add more reschedule cases to prio_changed_dl()

- **Commit:** 7ea98dfa44917a201e76d4fe96bf61d76e60f524
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline scheduling

## Bug Description

When a deadline task is replenished/boosted by another task (e.g., via rt_mutex_setprio) after being throttled, and the current task on that CPU is not a deadline task (e.g., idle), the scheduler fails to issue a reschedule. This causes the idle task to continue running instead of switching to the boosted deadline task, resulting in a system stall until an unrelated event sets TIF_NEED_RESCHED.

## Root Cause

The `prio_changed_dl()` function only called `resched_curr()` when the current task was a deadline task. When a deadline task was throttled (and current became idle), boosting another queued deadline task via `prio_changed_dl()` failed to trigger a reschedule because the check for task_current didn't handle the case where current is a non-deadline task like idle.

## Fix Summary

The fix restructures `prio_changed_dl()` to always check if a task is queued, and then unconditionally checks for reschedule conditions in two cases: (1) if the task is current and has an earlier deadline than the earliest deadline task, or (2) if the task is not current but either the current task is not a deadline task or the boosted task has an earlier deadline. This ensures reschedules are issued when a deadline task is boosted while the system is running an idle or non-deadline task.

## Triggering Conditions

This bug requires:
1. **Multi-CPU system** with at least one deadline task p0 owning an rt_mutex M
2. **Task p0 depletes runtime** and gets throttled, causing the runqueue to switch to idle 
3. **Another deadline task p1** on a different CPU blocks on mutex M
4. **rt_mutex_setprio()** boosts/replenishes p0 via `prio_changed_dl()`
5. **Current task is non-deadline** (e.g., idle) when the boost occurs
6. The original `prio_changed_dl()` only checked `dl_task(rq->curr)` before issuing `resched_curr()`, causing the boosted deadline task to remain unscheduled while idle continues running.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). Setup:
1. Use `kstep_task_create()` to create 2 deadline tasks (p0, p1) on CPUs 1-2
2. Configure deadline scheduling with `kstep_task_dl_params()` and a shared rt_mutex
3. In `run()`: Pin p0 to CPU 1, have it acquire mutex and deplete runtime via `kstep_tick_repeat()`  
4. Monitor p0 throttling via `on_tick_end()` callback checking `task_on_rq_queued(p0)`
5. Once p0 throttled and CPU 1 runs idle, pin p1 to CPU 2 and block on same mutex
6. This triggers `rt_mutex_setprio()` boosting p0, calling `prio_changed_dl()`
7. Check via `kstep_output_curr_task()` in callbacks if CPU 1 remains on idle instead of switching to boosted p0
8. Bug detected if idle continues running despite p0 being runnable and boosted
