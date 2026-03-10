# sched/rt: Fix RT utilization tracking during policy change

- **Commit:** fecfcbc288e9f4923f40fd23ca78a6acdc7fdf6c
- **Affected file(s):** kernel/sched/rt.c
- **Subsystem:** RT (Real-Time scheduler)

## Bug Description

When a running task changes its scheduling policy to RT, the RT utilization tracking structure (`avg_rt`) is not updated at the moment of policy change. Later, when the task is dequeued, `put_prev_task_rt()` updates the utilization based on a stale `last_update_time`, causing a huge spike in the RT utilization signal. This stale utilization measurement negatively impacts CPU capacity calculations, which depend on `avg_rt`, resulting in significant scheduler misbehavior until the signal recovers after several milliseconds.

## Root Cause

The function `set_next_task_rt()` normally updates RT utilization when the runqueue begins executing RT tasks. However, when a currently running task changes its policy to RT via `switched_to_rt()`, the task is already executing, so `set_next_task_rt()` is never called. This leaves `avg_rt` with an outdated `last_update_time`. When the task later dequeues via `put_prev_task_rt()`, it calculates utilization using this stale timestamp, producing an erroneous spike.

## Fix Summary

The fix adds an explicit check in `switched_to_rt()` to detect when a running task is switching to RT policy. In this case, `update_rt_rq_load_avg()` is called immediately to update the utilization tracking with the current time, ensuring `last_update_time` is fresh before the task later dequeues.
