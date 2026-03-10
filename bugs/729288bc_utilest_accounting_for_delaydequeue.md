# Fix util_est accounting for DELAY_DEQUEUE

- **Commit:** 729288bc68560b4d5b094cb7a6f794c752ef22a2
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

Delayed tasks that are migrating between runqueues or undergoing SAVE/RESTORE dequeue/enqueue cycles are incorrectly included in the utilization estimation (util_est) accounting. This causes the CPU frequency scaling subsystem to make frequency selection decisions based on inflated utilization estimates, since temporary task dequeue/enqueue operations that are part of migration or context-save/restore are counted as actual utilization changes.

## Root Cause

The `util_est_enqueue()` and `util_est_dequeue()` functions are called unconditionally for all tasks, regardless of whether the task is in a delayed state and undergoing transient dequeue/enqueue operations. When tasks with `sched_delayed` set are migrated between runqueues or saved/restored, they are transiently removed and re-added to the runqueue. These temporary operations trigger util_est updates that shouldn't count toward the utilization estimate, since the task is not actually changing its execution state—it's just being moved.

## Fix Summary

The fix adds a conditional check before calling `util_est_enqueue()` and `util_est_dequeue()` to exclude delayed tasks that are either migrating between runqueues or in a SAVE/RESTORE dequeue/enqueue cycle. This prevents incorrect utilization estimation updates during these transient state changes and allows frequency scaling decisions to be made on accurate utilization data.

## Triggering Conditions

The bug triggers when a task meets all of these conditions:
- Task has `sched_delayed` flag set (indicating delayed dequeue behavior)
- Task is either migrating between runqueues (`task_on_rq_migrating()` returns true) OR undergoing SAVE/RESTORE dequeue/enqueue cycle (`ENQUEUE_RESTORE`/`DEQUEUE_SAVE` flags set)
- The `enqueue_task_fair()` or `dequeue_task_fair()` functions are called during these transient operations

This occurs during load balancing when tasks migrate between CPUs, or during context save/restore operations where tasks are temporarily dequeued and re-enqueued. The util_est accounting incorrectly treats these transient operations as actual utilization changes, inflating the CPU frequency scaling decisions.

## Reproduce Strategy (kSTEP)

Use at least 3 CPUs (CPU 0 reserved for driver). In `setup()`:
- Create multiple CFS tasks with `kstep_task_create()`
- Pin tasks to different CPUs initially to create load imbalance
- Set up CPU topology with `kstep_topo_init()` and `kstep_topo_apply()`

In `run()`:
- Monitor util_est values before migration using direct kernel structure access
- Force task migration by re-pinning tasks with `kstep_task_pin()` to different CPUs
- Use `kstep_tick_repeat()` to allow load balancer to process migrations
- Log util_est values during and after migration operations

Use `on_sched_balance_begin()` and `on_sched_balance_selected()` callbacks to detect migration events. Check if delayed tasks show util_est changes during migration by comparing pre/post migration util_est values. The bug manifests as non-zero util_est deltas for tasks that should have no accounting during transient dequeue/enqueue cycles.
