# sched/fair: Fix unthrottle_cfs_rq() for leaf_cfs_rq list

- **Commit:** 39f23ce07b9355d05a64ae303ce20d1c4b92b957
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

The `unthrottle_cfs_rq()` function had incomplete leaf_cfs_rq list maintenance when handling throttled cfs_rq entities. During the unthrottling process, when the code encountered a throttled cfs_rq and broke from the loop, it would skip updating the leaf list for intermediate cfs_rq entities in the hierarchy. This incomplete maintenance could trigger assertion failures (`assert_list_leaf_cfs_rq()`) that validate the integrity of the leaf list.

## Root Cause

The original code used a single loop with a flag (`enqueue = 0`) to conditionally enqueue entities, and would break early when encountering a throttled cfs_rq. This approach left intermediate parent cfs_rq entities unadded to the leaf list if a throttle condition was encountered before reaching the root. The function did not properly ensure that all cfs_rq entities in the hierarchy were added back to the leaf list in all code paths, particularly when throttled entities interrupted the update sequence.

## Fix Summary

The fix splits the unthrottling logic into two separate loops and a final leaf list cleanup loop. The first loop unconditionally enqueues entities and updates counts until hitting a throttled cfs_rq (using `goto` for early exit). The second loop handles load average updates and ensures parent cfs_rq entities are added back to the leaf list when needed via `throttled_hierarchy()` check. A final loop unconditionally repairs the leaf list for all remaining entities in the hierarchy, ensuring complete and correct leaf list maintenance regardless of where throttling occurs.

## Triggering Conditions

The bug triggers when unthrottling a CFS bandwidth-constrained task group with a multi-level cgroup hierarchy where intermediate parent cfs_rq entities are throttled. The critical sequence requires: (1) A task group hierarchy with bandwidth limits, (2) Multiple levels of cfs_rq entities where parents get throttled while children remain unthrottled, (3) An unthrottle operation that encounters a throttled parent cfs_rq before reaching the root, causing the original single-loop logic to break early with `enqueue=0`, and (4) Incomplete leaf list maintenance for intermediate entities, potentially triggering `assert_list_leaf_cfs_rq()` failures that validate the integrity of the per-rq leaf cfs_rq list used by the load balancer.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs to create a nested cgroup hierarchy with CFS bandwidth limits that trigger throttling. In setup(), create nested cgroups with `kstep_cgroup_create()` and set restrictive bandwidth limits via `kstep_cgroup_write()` for "cpu.cfs_period_us" and "cpu.cfs_quota_us". Create tasks with `kstep_task_create()` and assign them to different hierarchy levels using `kstep_cgroup_add_task()`. In run(), generate load with `kstep_tick_repeat()` to trigger bandwidth throttling of parent cgroups while leaving child cfs_rq entities active. Then trigger unthrottling by pausing load generation and calling `kstep_tick_repeat()` to allow bandwidth replenishment. Use assertion checking callbacks or `kstep_fail()` to detect leaf list corruption by validating the per-rq cfs_rq list integrity during the unthrottling process, particularly watching for incomplete list maintenance when intermediate throttled entities interrupt the hierarchy walk.
