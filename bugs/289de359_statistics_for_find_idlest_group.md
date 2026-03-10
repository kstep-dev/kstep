# sched/fair: Fix statistics for find_idlest_group()

- **Commit:** 289de35984815576793f579ec27248609e75976e
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

During task wakeup (fork/exec), the scheduler gathers group statistics in `update_sg_wakeup_stats()` to find the idlest group for task placement. However, the `group_weight` field of the statistics structure was not being initialized. This caused groups to be incorrectly classified as "fully busy" with zero running tasks when utilization was high enough, leading to suboptimal scheduling decisions during fork and exec operations.

## Root Cause

The `sgs->group_weight` variable was uninitialized in the `update_sg_wakeup_stats()` function. The `group_classify()` function uses `group_weight` to determine a group's classification state. Without this field being set, the classification logic could produce incorrect results, particularly when calculating whether a group is fully busy based on utilization metrics alone, independent of the actual number of running tasks.

## Fix Summary

The fix adds a single initialization line `sgs->group_weight = group->group_weight;` in the `update_sg_wakeup_stats()` function before calling `group_classify()`. This ensures the group weight statistic is properly populated when evaluating scheduler groups, allowing correct classification and more optimal task placement decisions.

## Triggering Conditions

The bug triggers during task wakeup (fork/exec) when the scheduler calls `find_idlest_group()` to select the best CPU group for task placement. Specifically, it occurs when:
- A scheduler group has zero running tasks but high utilization (from recently completed work)  
- The uninitialized `sgs->group_weight` contains garbage data (typically 0)
- The `group_classify()` function incorrectly classifies this group as `group_fully_busy` instead of idle
- This causes suboptimal task placement as truly idle groups are overlooked in favor of "busy" groups
- The bug is most evident in multi-socket NUMA systems with multiple scheduler groups
- High CPU utilization followed by task completion creates the ideal conditions for misclassification

## Reproduce Strategy (kSTEP)

Setup a NUMA topology with 2 socket groups (CPUs 1-2 and 3-4) and create high utilization followed by task completion:
- Use 4 CPUs minimum (CPU 0 reserved for driver)
- Call `kstep_topo_init()`, set NUMA domains with `kstep_topo_set_node()` for separate socket groups
- Create multiple tasks and pin them to specific CPUs to generate high utilization
- Use `kstep_tick_repeat()` to accumulate utilization, then pause/complete tasks to leave high util but zero running tasks
- Create a new task and call `kstep_task_wakeup()` to trigger `find_idlest_group()` path
- Monitor with `on_sched_softirq_begin()` callback to observe group classification decisions
- Check if the new task is placed on the "idle" high-utilization group vs. truly idle groups
- Log group statistics and classification results to detect incorrect "fully busy" classification of idle groups
