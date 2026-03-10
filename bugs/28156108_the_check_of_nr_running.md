# sched: Fix the check of nr_running at queue wakelist

- **Commit:** 28156108fecb1f808b21d216e8ea8f0d205a530c
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The condition for offloading task activation to the wakelist when WF_ON_CPU is set incorrectly checks `rq->nr_running <= 1` instead of `rq->nr_running == 0`. This can cause unnecessary task stacking or incorrect task offloading decisions, leading to suboptimal scheduling behavior and potential performance degradation.

## Root Cause

The bug stems from an incorrect understanding of the ordering guarantees between p->on_rq and p->on_cpu. By the time ttwu_queue_cond() observes WF_ON_CPU (p->on_cpu is set), the task has already been deactivated and accounted out of rq->nr_running via deactivate_task() in __schedule(). Therefore, when the task is the only runnable task, nr_running should be exactly 0, not <= 1. The original condition of `<= 1` fails to account for this ordering property.

## Fix Summary

The fix changes the condition from `rq->nr_running <= 1` to `!rq->nr_running` (which is equivalent to `== 0`) to correctly reflect the accounting semantics. The fix also adds clarifying comments explaining the ordering guarantees between p->on_rq and p->on_cpu to prevent future misunderstandings.

## Triggering Conditions

The bug triggers in `ttwu_queue_cond()` when all of the following conditions are met:
- A task is waking up another task with WF_ON_CPU flag set (wakee is currently descheduling)
- The target CPU shares cache with the current CPU (`cpus_share_cache()` returns true)
- The target CPU has exactly 1 task in `nr_running` (after the descheduling task is accounted out)
- The buggy condition `nr_running <= 1` incorrectly evaluates to true, causing unnecessary wakelist queueing
- This leads to suboptimal task activation timing as the wakelist mechanism is used when direct activation would be more appropriate

## Reproduce Strategy (kSTEP)

Use 2-3 CPUs sharing the same cache domain. Create two tasks on the target CPU (CPU 1):
- `kstep_task_create()` for task A (will deschedule) and task B (will wake task A)
- Pin both tasks to CPU 1 with `kstep_task_pin(task, 1, 1)`
- In `run()`: Wake both tasks, let task A run briefly with `kstep_tick_repeat(5)`
- Pause task A with `kstep_task_pause()` to trigger descheduling
- Have task B attempt to wake task A with `kstep_task_wakeup()` 
- Use `on_tick_begin()` callback to monitor `cpu_rq(1)->nr_running` and log when it equals 1
- Check for incorrect wakelist usage by monitoring task activation paths
- The bug manifests as unnecessary wakelist queueing when nr_running == 1 instead of only when nr_running == 0
