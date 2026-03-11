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

## Triggering Conditions

The bug requires a task that is currently running (not just runnable) on a CPU to change its scheduling policy from CFS to RT while executing. The key sequence is:
- A CFS task must be actively running (current task) on a CPU  
- The task's scheduling policy changes to RT via `sched_setscheduler()` or similar syscalls
- `switched_to_rt()` is called but does not update `avg_rt.last_update_time` (buggy behavior)
- The task continues running as an RT task, accumulating runtime
- Later, when the task is dequeued (sleeps, exits, or policy change), `put_prev_task_rt()` calculates utilization using the stale timestamp from before the policy switch
- This produces a utilization spike proportional to the time gap between policy change and dequeue

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). Create a CFS task on CPU 1, switch it to RT while running, then observe the utilization spike:
- **Setup:** Create a single CFS task with `kstep_task_create()` and pin it to CPU 1 with `kstep_task_pin(task, 1, 1)`
- **Initial phase:** Wake the CFS task with `kstep_task_wakeup(task)` and let it run for several ticks using `kstep_tick_repeat(10)` to ensure it becomes the current running task
- **Policy switch:** While the task is running, change it to RT policy using `kstep_task_fifo(task)` - this triggers the buggy `switched_to_rt()` path
- **RT execution:** Continue running with `kstep_tick_repeat(20)` to accumulate RT runtime with stale utilization tracking  
- **Trigger spike:** Dequeue the task using `kstep_task_pause(task)` to trigger `put_prev_task_rt()` which computes utilization from the stale timestamp
- **Detection:** Monitor RT utilization before/after the pause using custom logging in `on_tick_end()` callback to detect the characteristic spike in `cpu_rq(1)->rt.avg_rt.util_avg`
