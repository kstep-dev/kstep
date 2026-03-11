# Fix inaccurate h_nr_runnable accounting with delayed dequeue

- **Commit:** 3429dd57f0deb1a602c2624a1dd7c4c11b6c4734
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair Scheduler)

## Bug Description

When a task blocks and is dequeued, both `dequeue_entity()` and `set_delayed()` incorrectly decrement `h_nr_runnable` for the same task when the task's cfs_rq entity is delayed. This double-decrement causes `h_nr_runnable` to become negative, triggering a `SCHED_WARN_ON()` consistently in wakeup-intensive workloads like hackbench running in a cgroup. The incorrect accounting can influence PELT (Per-Entity Load Tracking) calculations until the value self-corrects when the task is later picked or requeued.

## Root Cause

The `set_delayed()` and `clear_delayed()` functions adjust `h_nr_runnable` for both task entities and cfs_rq entities without distinction. However, when a task entity is delayed and takes the early-return path in `dequeue_entities()`, the hierarchy accounting is handled by `dequeue_entity()`. If the cfs_rq corresponding to that task is also delayed, `set_delayed()` then adjusts the same hierarchy again, causing double-decrement of the counters. The bug occurs because `set_delayed()` and `clear_delayed()` should only adjust h_nr_runnable for actual task entities, not for entities representing cfs_rq structures which have no tasks queued on them.

## Fix Summary

The fix modifies `set_delayed()` and `clear_delayed()` to check if the entity is a task using `entity_is_task(se)` and only adjust h_nr_runnable when processing actual task entities. For cfs_rq entities (which have no tasks), the accounting is left to the `dequeue_entities()` and `enqueue_task_fair()` loops, eliminating the double-decrement.

## Triggering Conditions

The bug is triggered when tasks within cgroups experience frequent wake-up/block cycles that cause delayed dequeue scenarios. Specifically:
- A cgroup hierarchy must exist with tasks that can trigger delayed dequeue (EEVDF delayed dequeue feature)
- Tasks must frequently block (sleep) and wake up, creating wakeup-intensive workloads 
- When a task blocks, its `dequeue_entity()` returns true, and if the corresponding cfs_rq entity is also delayed, both `dequeue_entity()` and `set_delayed()` decrement `h_nr_runnable` for the same task
- The double-decrement causes `h_nr_runnable` to become negative, triggering `SCHED_WARN_ON()` in the dequeue path
- The race occurs in the CFS fair scheduler when processing entity hierarchy accounting during delayed dequeue operations

## Reproduce Strategy (kSTEP)

Create a cgroup hierarchy with tasks performing frequent wake/sleep cycles to trigger the double-decrement bug:
- Setup: Use at least 2 CPUs (CPU 0 reserved for driver). Create multiple cgroups using `kstep_cgroup_create("groupA")` and `kstep_cgroup_create("groupB")`
- Create multiple tasks with `kstep_task_create()` and assign them to different cgroups using `kstep_cgroup_add_task("groupA", task_pid)`
- In `run()`: Repeatedly wake tasks with `kstep_task_wakeup()`, let them run briefly with `kstep_tick()`, then force them to block with `kstep_task_pause()`
- Use `on_tick_begin` callback to monitor and log `h_nr_runnable` values for each cfs_rq in the hierarchy
- Monitor for negative `h_nr_runnable` values or kernel warnings during the wake/block cycles
- Detection: Log when `h_nr_runnable` becomes negative or check for scheduler warnings in kernel logs indicating the double-decrement bug
