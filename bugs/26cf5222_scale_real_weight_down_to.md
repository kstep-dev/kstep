# sched: Avoid scale real weight down to zero

- **Commit:** 26cf52229efc87e2effa9d788f9b33c40fb3358a
- **Affected file(s):** kernel/sched/sched.h
- **Subsystem:** CFS

## Bug Description

When a cgroup has very small shares relative to its parent in a hierarchy, the scheduler incorrectly allocates CPU resources due to weight scaling down to zero. In the reported case, a cgroup C with 1024 shares under parent B (shares=2) under parent A (shares=102400) receives significantly fewer CPU resources than a sibling group F with equivalent shares but better hierarchy positioning, despite being entitled to more CPU time.

## Root Cause

The `scale_load_down()` macro simply right-shifts weights without bounds. When calculating group shares via `calc_group_shares()`, if a parent cgroup's `cfs_rq->load.weight` is very small, scaling it down results in zero. This causes the share calculation `(tg_shares * load) / tg_weight` to become zero even though the cgroup has non-zero shares, losing critical information about the actual CPU entitlement and leading to incorrect scheduling decisions.

## Fix Summary

The fix modifies `scale_load_down()` to ensure that if the input weight is non-zero, the scaled-down result is at least 2UL (MIN_SHARES). This prevents the loss of information when scaling very small weights, allowing `calc_group_shares()` to correctly calculate shares for deeply nested cgroups with low weight values.

## Triggering Conditions

This bug requires deeply nested cgroup hierarchies where intermediate cgroups have very low share values that cause weight scaling to zero. Specifically: a parent cgroup A with high shares containing an intermediate cgroup B with extremely low shares (e.g., 2) containing a child cgroup C with normal shares. When `calc_group_shares()` is called for group A, the `cfs_rq->load.weight` becomes very small due to group B's low weight. The `scale_load_down()` function then scales this small weight to zero, causing the share calculation `(tg_shares * load) / tg_weight` to become zero despite the cgroup having legitimate CPU entitlement. This occurs during periodic load balancing when the scheduler recalculates group shares based on current load conditions.

## Reproduce Strategy (kSTEP)

Create the exact cgroup hierarchy from the bug report using kSTEP: cgroup A (shares=102400) → B (shares=2) → C (shares=1024) and D (shares=1024) → E (shares=1024) → F (shares=1024). Use `kstep_cgroup_create()` to establish the hierarchy and `kstep_cgroup_set_weight()` to configure shares. Create CPU-intensive tasks with `kstep_task_create()` and assign them to groups C and F via `kstep_cgroup_add_task()`. Run tasks for sufficient ticks with `kstep_tick_repeat(200)` to allow load balancing. Use `on_sched_group_alloc` callback to log when task groups are created and monitor `calc_group_shares()` calculations through kernel tracing. The bug is triggered when group C (which should receive more CPU due to A's high shares) gets fewer resources than group F. Detection involves comparing CPU time allocation between the groups - group F incorrectly receiving more CPU time indicates the bug is present.
