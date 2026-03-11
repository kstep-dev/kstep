# sched/deadline: Fix server stopping with runnable tasks

- **Commit:** ca1e8eede4fc68ce85a9fdce1a6c13ad64933318
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

The deadline server can incorrectly stop (transition to idle) even when fair scheduler tasks are runnable and waiting to be serviced. This occurs in two scenarios: when the server is set to idle and a task wakes up, the server stops immediately; or when a task wakes up, the server sets itself idle and stops right away. This results in fair task starvation and incorrect scheduling behavior.

## Root Cause

The bug stems from two issues: (1) the idle detection logic used `rq->curr == rq->idle` which doesn't account for pending fair tasks that may be enqueued but not yet running; and (2) the `dl_defer_idle` flag wasn't being cleared when the server starts, allowing it to immediately transition back to idle despite waking fair tasks. This was a regression from a previous deadline server fix attempt.

## Fix Summary

The fix changes idle detection from `rq->curr == rq->idle` to `idle_rq(rq)`, which properly accounts for pending tasks. Additionally, it clears the `dl_defer_idle` flag at the start of `dl_server_start()`, ensuring the server doesn't prematurely stop after being awakened. The state machine diagram is updated to reflect the correct transition paths.

## Triggering Conditions

The bug occurs in deadline server's idle detection logic within `update_curr_dl_se()` during fair task scheduling. Two specific scenarios trigger the premature server stopping: (1) When the server is already set to idle state and a fair task wakes up, the flawed idle check (`rq->curr == rq->idle`) misses pending runnable tasks in the runqueue, causing the server to stop immediately; (2) When a fair task wakes up and the server begins transitioning to idle, the `dl_defer_idle` flag remains set from previous operations, causing immediate termination despite new fair work. The bug requires fair tasks to be managed by a deadline server (typically for bandwidth control) and specific timing where task wakeups coincide with server idle transitions.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs. In `setup()`, create a deadline server task with `kstep_task_create()` and set it as deadline server, plus create 2-3 fair tasks with `kstep_task_create()`. In `run()`, first start the deadline server with appropriate deadline parameters, then wake fair tasks with `kstep_task_wakeup()` and let them run briefly via `kstep_tick_repeat()`. Next, pause all fair tasks with `kstep_task_pause()` to trigger server idle transition, then immediately wake one fair task with `kstep_task_wakeup()`. Use `on_tick_end()` callback to monitor server state transitions and track `dl_defer_idle` flag status. The bug manifests as the deadline server stopping (becoming inactive) despite fair tasks being runnable, detectable by examining server active state and fair task queue lengths in callbacks.
