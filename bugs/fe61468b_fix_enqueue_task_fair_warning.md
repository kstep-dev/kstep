# sched/fair: Fix enqueue_task_fair warning

- **Commit:** fe61468b2cbc2b7ce5f8d3bf32ae5001d4c434e9
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When CFS bandwidth control is enabled and a parent CFS run queue is throttled, the leaf list maintenance becomes incomplete. Tasks enqueued in throttled branches may not be re-added to the leaf list because the condition `nr_running == 1` alone is insufficient—a throttled CFS RQ can have `nr_running > 1` yet still need re-addition. Additionally, the unthrottle operation can terminate early when a parent is throttled, leaving remaining entities' CFS RQs out of the leaf list, violating list invariants.

## Root Cause

The `enqueue_entity()` function only adds a CFS RQ to the leaf list when `nr_running == 1`, but under CFS bandwidth control, a throttled CFS RQ that was removed from the leaf list may have `nr_running > 1` when a task is enqueued into it, causing the re-addition to be skipped. Similarly, in `unthrottle_cfs_rq()`, the main loop can break when it encounters a throttled parent, preventing proper re-addition of remaining ancestor CFS RQs to the leaf list.

## Fix Summary

The fix unconditionally calls `list_add_leaf_cfs_rq()` when CFS bandwidth is used during entity enqueue, ensuring throttled branches are properly re-added. In `unthrottle_cfs_rq()`, a separate loop is added after the main iteration to ensure all remaining entities in the chain are added to the leaf list, maintaining list completeness regardless of throttling state.

## Triggering Conditions

This bug requires CFS bandwidth control to be active with hierarchical task groups where parent groups can be throttled. The issue occurs when: (1) A parent CFS RQ is throttled, causing it and its children to be removed from the leaf list while maintaining `nr_running > 1`, (2) A new task is enqueued into a child CFS RQ within the throttled branch, triggering `enqueue_entity()` which skips `list_add_leaf_cfs_rq()` because `nr_running != 1`, (3) During unthrottle operations, the traversal encounters a throttled parent and breaks early, leaving ancestor CFS RQs out of the leaf list. This creates inconsistent leaf list state where some CFS RQs with runnable tasks are missing from the list.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. Create a hierarchical cgroup structure with bandwidth limits using `kstep_cgroup_create()` and `kstep_cgroup_write()` to set quota/period values that will trigger throttling. Create multiple tasks with `kstep_task_create()` and assign them to nested cgroups using `kstep_cgroup_add_task()`. Use `kstep_tick_repeat()` to consume bandwidth and trigger parent group throttling. While throttled, enqueue additional tasks to child groups and verify leaf list corruption using callbacks like `on_tick_begin()` to check CFS RQ leaf list state. The bug manifests as missing CFS RQs from the leaf list despite having runnable tasks, which can be detected by comparing expected vs actual leaf list contents during scheduling operations.
