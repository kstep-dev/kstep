# sched/fair: Prevent cfs_rq from being unthrottled with zero runtime_remaining

- **Commit:** 956dfda6a70885f18c0f8236a461aa2bc4f556ad
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

When a task group with CPU bandwidth quotas is initialized and throttled tasks migrate to its cfs_rq before any tasks are enqueued, the cfs_rq can enter a chaotic mixed state during unthrottling. This occurs when the cfs_rq is initialized with zero runtime_remaining, and upon unthrottling in a throttled hierarchy, the first task enqueue triggers an immediate re-throttle that leaves tasks stranded in the limbo list, violating the invariant that a throttled cfs_rq should have an empty limbo list.

## Root Cause

In tg_set_cfs_bandwidth(), the cfs_rq->runtime_remaining is initialized to 0 when quotas are enabled. When unthrottle_cfs_rq() is called during the unthrottle path and checks if runtime_remaining <= 0, the immediate enqueue in tg_unthrottle_up() triggers check_enqueue_throttle(), which can throttle the cfs_rq again if no runtime is available. This unexpected throttle on the unthrottle path leaves throttled tasks in the limbo list, triggering a WARN_ON in tg_throttle_down().

## Fix Summary

The fix grants 1 nanosecond of runtime_remaining when initializing a cfs_rq with quotas enabled in tg_set_cfs_bandwidth(). This ensures the cfs_rq is never entered with zero runtime during unthrottling, preventing the re-throttle condition from being triggered on the unthrottle path. Additionally, comments in unthrottle_cfs_rq() are updated to reflect this guarantee.

## Triggering Conditions

This bug requires a nested cgroup hierarchy where both parent and child have CPU bandwidth quotas enabled. The child cfs_rq must be initialized with zero runtime_remaining while being unthrottled. Before any tasks are enqueued to the child cfs_rq, throttled tasks must migrate to it (e.g., due to task group changes) while the parent cgroup is throttled, causing these tasks to be placed in the child's limbo list via enqueue_throttled_task(). The bug triggers when the parent is later unthrottled: tg_unthrottle_up() attempts to enqueue tasks from the limbo list, but the first enqueue triggers check_enqueue_throttle() with zero runtime_remaining, immediately re-throttling the cfs_rq and leaving remaining tasks stranded in the limbo list, violating the invariant that throttled cfs_rqs should have empty limbo lists.

## Reproduce Strategy (kSTEP)

Create a nested cgroup hierarchy: parent group A with quotas, child group C with quotas (CPU count: 2+ needed, CPU 0 reserved for driver). In setup(), use kstep_cgroup_create("groupA") and kstep_cgroup_create("groupA/groupC"), then set bandwidth quotas with kstep_cgroup_write() on cpu.cfs_quota_us and cpu.cfs_period_us for both groups. Create multiple tasks with kstep_task_create() and initially place them in groupA. In run(), first throttle groupA by consuming its quota, then migrate tasks to groupC using kstep_cgroup_add_task() while groupA remains throttled (tasks go to groupC's limbo list). Use on_sched_softirq_end callback to detect when groupA becomes unthrottled and monitor the limbo list state. The bug manifests as a WARN_ON in tg_throttle_down() due to non-empty limbo list when groupC gets re-throttled during the unthrottle path. Detection involves checking for kernel warnings and verifying limbo list state inconsistencies.
