# sched/rt: Fix Deadline utilization tracking during policy change

- **Commit:** d7d607096ae6d378b4e92d49946d22739c047d4c
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

When a running task changes its scheduling policy to DEADLINE, the deadline utilization tracking structure (`avg_dl`) is not updated, leaving it outdated. Later, when that task is dequeued, `put_prev_task_dl()` updates the utilization based on an incorrect `last_update_time`, causing a huge spike in the DL utilization signal. This impacts CPU capacity calculations and affects scheduler decision-making, with effects lasting several milliseconds.

## Root Cause

The `switched_to_dl()` function handles policy changes to deadline scheduling. Normally, `set_next_task_dl()` updates the load when the runqueue starts running DL tasks. However, when the *current* running task changes its policy to DL (`rq->curr == p`), `set_next_task_dl()` is never called for this transition, so the `avg_dl` structure never gets updated at that point. The stale `last_update_time` value then causes an incorrect calculation when the task is later dequeued.

## Fix Summary

The fix adds an explicit call to `update_dl_rq_load_avg()` in the `else` branch of `switched_to_dl()` when the current task changes to deadline policy. This ensures the utilization tracking structure is properly synchronized at the moment of policy change, before the task is later dequeued.

## Triggering Conditions

The bug occurs when a currently running task changes its scheduling policy to DEADLINE. Specifically:
- A task must be the current running task (`rq->curr == p`) on a CPU
- The task must be on the runqueue (`task_on_rq_queued(p) == true`)
- The task's scheduling policy is changed to SCHED_DEADLINE via `sched_setscheduler()`
- This triggers `switched_to_dl()` where `rq->curr == p`, taking the `else` branch
- The `avg_dl` structure's `last_update_time` becomes stale since `update_dl_rq_load_avg()` is not called
- Later when the task is dequeued or paused, `put_prev_task_dl()` calculates utilization using the stale timestamp
- This causes an incorrect utilization spike in the DL utilization tracking signal

## Reproduce Strategy (kSTEP)

Create a task, make it current, change to DEADLINE policy while running, then dequeue to trigger the bug:
- Use at least 2 CPUs (CPU 0 reserved for driver)
- In `setup()`: Create a CFS task with `kstep_task_create()`, pin to CPU 1 with `kstep_task_pin()`
- In `run()`: Wake the task with `kstep_task_wakeup()`, run several ticks with `kstep_tick_repeat()` until it becomes current
- While the task is running, use kSTEP's internal policy change mechanism or syscall simulation to change the task's policy to SCHED_DEADLINE
- Continue running for several ticks to accumulate runtime and let time advance
- Pause the task with `kstep_task_pause()` to trigger `put_prev_task_dl()` 
- Use `on_tick_begin()` callback to monitor and log the DL utilization values from `cpu_rq(1)->dl.avg_dl`
- Detect the bug by observing an abnormal spike in DL utilization when the task is dequeued
- Compare utilization values before policy change, after policy change, and after dequeuing to identify the spike
