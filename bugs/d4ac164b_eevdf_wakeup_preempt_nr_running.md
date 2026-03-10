# sched/eevdf: Fix wakeup-preempt by checking cfs_rq->nr_running

- **Commit:** d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

When a system has both CFS and RT tasks running, commit 85e511df3cec caused unnecessary preemptions by checking the total task count (`rq->nr_running`) instead of just fair scheduler tasks (`cfs_rq->nr_running`). This resulted in a 2.2% increase in involuntary context switches in workloads like hackbench that are sensitive to over-scheduling.

## Root Cause

The check in `update_curr()` was changed to use `rq->nr_running` to lower the bar for preemption when shorter slices are introduced. However, this causes reschedules even when only one CFS task exists alongside other RT tasks, leading to unnecessary scheduling overhead and performance degradation.

## Fix Summary

Revert the check from `if (rq->nr_running == 1)` back to `if (cfs_rq->nr_running == 1)` to only consider fair scheduler tasks when deciding whether to skip reschedule logic, preventing spurious preemptions in mixed workloads.

## Triggering Conditions

The bug occurs in `update_curr()` within the CFS scheduler when:
- At least one CFS task and one non-CFS task (RT/DL) are present on the same CPU
- The `update_deadline()` function returns true (indicating the current task exceeded its slice)
- The system is running with the buggy logic that checks `rq->nr_running == 1` instead of `cfs_rq->nr_running == 1`
- During normal CFS task execution when `update_curr()` is called (e.g., on tick, task switches)
- Results in spurious `resched_curr()` calls even when only one CFS task exists, causing unnecessary preemptions and increased context switches

## Reproduce Strategy (kSTEP)

Setup requires at least 2 CPUs (CPU 0 reserved for driver). Create mixed CFS/RT workload:
1. In `setup()`: Create one CFS task and one RT task using `kstep_task_create()` and `kstep_task_fifo()`
2. Pin both tasks to CPU 1 with `kstep_task_pin()` to ensure they compete on the same runqueue
3. In `run()`: Wake both tasks with `kstep_task_wakeup()` and run with `kstep_tick_repeat()`
4. Use `on_tick_end()` callback to monitor involuntary context switches by checking task state transitions
5. Log `rq->nr_running` vs `cfs_rq->nr_running` in callback to detect when buggy condition occurs
6. Bug detected when: `rq->nr_running > 1` and `cfs_rq->nr_running == 1` but `resched_curr()` still triggered
7. Use `kstep_output_curr_task()` and custom logging to track excessive preemptions in CFS task execution
