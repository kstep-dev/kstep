# sched/fair: Fix enqueue_task_fair() warning some more

- **Commit:** b34cb07dde7c2346dec73d053ce926aeaa087303
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The enqueue_task_fair() function generates a warning when rq->tmp_alone_branch does not equal &rq->leaf_cfs_rq_list during certain scheduling scenarios. This occurs when the first for_each_sched_entity loop exits early due to on_rq status, incompletely updating the leaf CFS RQ list. Subsequently, the second for_each_sched_entity loop modifies the sched_entity pointer, causing the list fixup code to fail because it operates on the wrong entity reference.

## Root Cause

When a CFS run queue has throttled parents and the first traversal loop exits early (due to on_rq), the se pointer no longer references the sched_entity that broke out of the loop. The subsequent list management code attempts to fix up the leaf list but operates on the wrong entity, leaving the throttled parent CFS RQ improperly added back to the list (already done by a parallel child hierarchy's enqueue). This causes the leaf list invariant to be violated.

## Fix Summary

The fix adds a throttled hierarchy check within the second for_each_sched_entity loop to detect when a parent has been throttled and removed from the list. When detected, list_add_leaf_cfs_rq() is called immediately to restore the correct leaf list state, ensuring the invariant is maintained regardless of the loop exit condition.

## Triggering Conditions

The bug requires a hierarchical cgroup setup with throttled parent cfs_rq and multiple parallel child task group enqueues. Specifically: (1) A task hierarchy where parent task groups can be throttled, (2) First enqueue loop exits early due to se->on_rq being true, leaving incomplete list updates, (3) Concurrent task enqueues in parallel child hierarchies modify the leaf_cfs_rq_list, (4) Second for_each_sched_entity loop operates on a different se pointer than the one that broke out of the first loop, and (5) The throttled parent cfs_rq gets improperly managed in the leaf list, causing rq->tmp_alone_branch != &rq->leaf_cfs_rq_list warning.

## Reproduce Strategy (kSTEP)

Create hierarchical task groups with throttling enabled using at least 3 CPUs. In setup(), use kstep_cgroup_create() to create nested cgroups "parent" and "parent/child", then kstep_cgroup_set_weight() to establish different weights. Create multiple tasks using kstep_task_create() and distribute them across the hierarchy with kstep_cgroup_add_task(). In run(), first establish throttling conditions by overloading the parent cgroup, then perform concurrent wakeups using kstep_task_wakeup() on tasks in parallel child hierarchies. Use on_tick_begin() callback to monitor cfs_rq states and check for the warning condition. Detect the bug by observing list inconsistencies in the leaf_cfs_rq_list or catching the specific warning message in kernel logs via TRACE_INFO() when the invariant is violated.
