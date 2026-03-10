# sched/deadline: Fix dl_server getting stuck

- **Commit:** 4ae8d9aa9f9dc7137ea5e564d79c5aa5af1bc45c
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** Deadline

## Bug Description

The deadline server can become stuck in a dequeued state while still marked as active (dl_se->dl_server_active). When this occurs, dl_server_start() returns without enqueueing the server, preventing it from running when RT tasks starve the CPU. This results in lockup warnings and CPU starvation, leaving the server "dead" with no timer and no enqueue path.

## Root Cause

The deadline server code was conditionally checking `dl_se->server_has_tasks()` to decide whether to replenish the server and stop it. However, this check is incorrect: the decision to start the zero-laxity timer should be independent of whether fair tasks currently exist. When no tasks are present at the moment of the bandwidth check, the server incorrectly skips the normal timer and enqueue path, leaving it stuck and unable to resume when tasks later become available.

## Fix Summary

The fix removes all uses of the `server_has_tasks()` callback and callback pointer mechanism from the deadline server code. This allows the bandwidth timer to unconditionally proceed to start the zero-laxity timer and enqueue the server, regardless of the current task queue state. The subsequent pick_task() call will then correctly determine whether to stop the server based on a full period of observation.

## Triggering Conditions

The bug requires a deadline server to become dequeued while still marked as active (`dl_se->dl_server_active = 1`). This occurs when:
- RT tasks are starving the CPU, preventing fair task execution
- The deadline server's bandwidth timer fires (`dl_server_timer()`) 
- At that exact moment, no fair tasks are present in the CFS runqueue (`server_has_tasks()` returns false)
- The server gets replenished and stopped via `dl_server_stopped()`, returning `HRTIMER_NO_RESTART`
- This leaves the server in a "dead" state: dequeued from the deadline runqueue but still marked as active, with no timer to restart it
- When fair tasks later arrive, `dl_server_start()` exits early due to the active flag, preventing enqueue

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved for driver). In `setup()`, create RT tasks on CPUs 1-2 and fair tasks that can migrate. In `run()`:
1. Use `kstep_task_fifo()` to create high-priority RT tasks that monopolize CPUs 1-2
2. Create fair tasks with `kstep_task_create()` and `kstep_task_cfs()` that initially run on CPU 3
3. Use `kstep_task_pause()` to temporarily remove all fair tasks right before anticipated server bandwidth refresh
4. Let RT tasks starve the system for multiple tick periods using `kstep_tick_repeat()`
5. Monitor server state in `on_tick_begin()`: log `dl_se->dl_server_active` and server queue status
6. Use `kstep_task_wakeup()` to reintroduce fair tasks after server becomes stuck
7. Detect the bug: server remains marked active but dequeued, fair tasks get no CPU time despite being runnable
8. Verify with lockup detection timeouts and CPU utilization measurements
