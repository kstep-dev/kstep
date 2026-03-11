# sched/deadline: Fix dl_server time accounting

- **Commit:** e636ffb9e31b4f7dde7fef5358669266b9ce02ec
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/fair.c, kernel/sched/idle.c, kernel/sched/sched.h
- **Subsystem:** Deadline (sched/deadline)

## Bug Description

The deadline server's time accounting was not following the standard scheduler pattern of updating the current task before making state changes. This caused incorrect runtime accounting for deadline tasks, particularly when the server timer fired or when the server was started. The `dl_server_update_idle_time()` function also had timing issues where it would not properly advance timestamps in all code paths, causing stale timestamps and incorrect idle time deltas to accumulate when defer mode was enabled.

## Root Cause

The bug occurred because `dl_server_timer()` and `dl_server_start()` were making state changes without first propagating the current task's pending runtime into the server through `update_curr()`. Additionally, `dl_server_update_idle_time()` only updated `p->se.exec_start` conditionally, leaving stale timestamps when defer mode was not active, which would then be incorrectly included when defer mode was later enabled. This violated the fundamental scheduler invariant that the current task's state must be fully accounted before state transitions.

## Fix Summary

The fix adds explicit `update_curr()` calls in `dl_server_timer()` and `dl_server_start()` to ensure the current task's runtime is propagated to the server before state changes. It refactors `dl_server_update_idle_time()` into a simpler `dl_server_update_idle()` function that only accounts idle time in defer mode, and adds proper `update_curr_idle()` implementation in the idle scheduler class to consistently track idle task time. These changes ensure the deadline server follows the standard pattern and maintains correct runtime accounting in all modes.

## Triggering Conditions

The bug triggers when deadline server operations (`dl_server_timer()` or `dl_server_start()`) occur without proper current task accounting. Key conditions:
- A deadline server (specifically `rq->fair_server`) is active with deferred mode enabled
- Tasks are running under the fair server, accumulating runtime in `se.exec_start` 
- Server timer fires or server starts/stops, causing state transitions before `update_curr()` propagates pending runtime
- Idle task transitions occur without proper timestamp updates, leaving stale `se.exec_start` values
- The race occurs in the deadline server code path in `kernel/sched/deadline.c` functions
- CPU load balancing or task enqueue/dequeue events that trigger `dl_server_start()`

## Reproduce Strategy (kSTEP)

Set up 2+ CPUs with deadline servers and trigger accounting races:
- **Setup**: Use `kstep_cgroup_create("fair_cg")` and configure fair server with defer mode enabled via sysctl
- **Task creation**: Create CFS tasks with `kstep_task_create()` and `kstep_cgroup_add_task()`  
- **Trigger conditions**: Use `kstep_task_pause()` followed by `kstep_task_wakeup()` to trigger `dl_server_start()`
- **Race timing**: Call `kstep_tick()` while tasks accumulate runtime, then trigger server events
- **Observation**: Use `on_tick_begin()` callback to log `rq->fair_server.runtime` and `curr->se.exec_start`
- **Detection**: Compare runtime values before/after server events - incorrect accounting shows as inconsistent deltas
- **Focus**: Monitor `dl_server_timer()` and idle task transitions for missing `update_curr()` calls
