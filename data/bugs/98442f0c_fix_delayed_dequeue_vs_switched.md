# sched: Fix delayed_dequeue vs switched_from_fair()

- **Commit:** 98442f0ccd828ac42e89281a815e9e7a97533822
- **Affected file(s):** kernel/sched/core.c, kernel/sched/ext.c, kernel/sched/fair.c, kernel/sched/sched.h, kernel/sched/syscalls.c
- **Subsystem:** core, fair scheduling, delayed dequeue

## Bug Description

When a task's scheduling class changes (e.g., from fair to RT or deadline), and the task has the `sched_delayed` flag set, the task could be improperly enqueued in the new scheduling class before the deferred dequeue operation completes. This race condition between class switching and delayed dequeue led to scheduling failures that were triggered once per ~10k CPU hours under RCU torture testing. The bug manifested as a task being left in an inconsistent state across scheduling classes.

## Root Cause

The `switched_from_fair()` hook was called after the task's scheduling class had already been changed, making it impossible to properly clean up the deferred dequeue state before the new class's enqueue logic executed. The hook-based architecture placed the deferred dequeue handling in the wrong execution order relative to the actual class transition, creating a window where the task could be enqueued in the new class while still having unresolved delayed dequeue state.

## Fix Summary

The fix moves the deferred dequeue handling outside of the hook system and into the class-switching code paths (`rt_mutex_setprio()` and `__sched_setscheduler()`) where it can execute before the class change. When a class change occurs and `sched_delayed` is set, the task is now explicitly dequeued with the `DEQUEUE_DELAYED` flag before the new scheduling class is assigned, ensuring proper cleanup before the new class's enqueue operations execute.

## Triggering Conditions

The bug requires a fair-scheduled task with the `sched_delayed` flag set to undergo a scheduling class transition (fair → RT/DL) before the deferred dequeue operation completes. The `sched_delayed` flag is typically set when a task becomes unrunnable but its dequeue is deferred for performance reasons. The race occurs in the priority inheritance code path (`rt_mutex_setprio()`) or explicit priority changes (`__sched_setscheduler()`). The timing window exists between when the new scheduling class is determined and when `switched_from_fair()` would execute the delayed dequeue cleanup. During this window, the new class's enqueue operations can execute on a task with unresolved delayed dequeue state, leaving the task inconsistently queued across scheduling classes.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). In `setup()`, create a fair task with `kstep_task_create()` and pin to CPU 1 with `kstep_task_pin()`. In `run()`, use `kstep_task_wakeup()` and `kstep_tick_repeat()` to run the task until it gets delayed dequeue state (task becomes unrunnable but dequeue deferred). Then immediately trigger a scheduling class change via `kstep_task_set_prio()` with RT priority (e.g., prio 50) to force fair→RT transition. Use `on_tick_begin()` callback to log task state: `p->se.sched_delayed`, `p->sched_class`, and `task_on_rq_queued(p)`. The bug manifests as the task having `sched_delayed=1` while being enqueued in the RT class, or inconsistent queue state across classes. Check for warnings/crashes from the scheduling core when the delayed dequeue cleanup executes after the class transition.
