# sched/pelt: Avoid underestimation of task utilization

- **Commit:** 50181c0cff31281b9f1071575ffba8a102375ece
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** PELT (Per-Entity Load Tracking), CFS

## Bug Description

When multiple tasks share a single CPU, a task's util_est (estimated utilization) can significantly decrease even though the task's actual computational needs remain unchanged. This occurs because util_est follows the task's observed maximum utilization, which decreases when CPU time is shared. The underestimated util_est causes the task to be provisioned with insufficient CPU resources, leaving it under-provisioned and degrading its performance.

## Root Cause

The util_est EWMA (Exponential Weighted Moving Average) is updated whenever a task wakes up from sleep. When a task shares the CPU with other threads, its measured utilization during execution decreases because it receives less CPU time per scheduling window. The existing update logic fails to distinguish between actual reduced computational needs versus reduced CPU allocation due to contention. This causes the EWMA to incorrectly trend downward, underestimating the task's true utilization needs.

## Fix Summary

A new check is added to detect when a task has more runnable work (indicated by `runnable_avg`) than what was recently executed (enqueued). When `(enqueued + UTIL_EST_MARGIN) < runnable_avg`, it indicates the task is waiting on the runqueue wanting more CPU time, so the EWMA update is skipped to prevent underestimation. This preserves the util_est at a higher level that better reflects the task's actual computational requirements, even when experiencing CPU contention.

## Triggering Conditions

The bug occurs in the PELT util_est update path (`util_est_update()` in `kernel/sched/fair.c`) when:
- Multiple tasks compete for CPU time on the same runqueue
- A task with established util_est wakes up and experiences CPU contention
- The task's `runnable_avg` exceeds `util_avg` due to waiting time on the runqueue
- The util_est EWMA gets updated with the artificially reduced utilization
- The condition `(ue.enqueued + UTIL_EST_MARGIN) < task_runnable(p)` is true but not detected
- This causes progressive underestimation of the task's true computational requirements

## Reproduce Strategy (kSTEP)

1. **Setup**: Use 2+ CPUs (CPU 0 reserved). Create two periodic tasks with similar characteristics
2. **Phase 1**: Run target task alone on CPU 1 using `kstep_task_pin(task_a, 1, 1)` and `kstep_task_wakeup(task_a)`
3. **Establish baseline**: Use `kstep_tick_repeat()` with alternating `kstep_task_pause()` and `kstep_task_wakeup()` to build stable util_est (~888)
4. **Phase 2**: Introduce contention by pinning second task to same CPU: `kstep_task_pin(task_b, 1, 1); kstep_task_wakeup(task_b)`
5. **Trigger bug**: Continue periodic wake/sleep cycles. Monitor task_a's `se.avg.util_est.enqueued`, `se.avg.util_avg`, and `se.avg.runnable_avg`
6. **Detection**: Use `on_tick_begin()` callback to log when `runnable_avg > util_avg + UTIL_EST_MARGIN` but util_est still decreases
7. **Verification**: Without fix, util_est drops to ~512. With fix, util_est remains high when runnable_avg indicates contention
