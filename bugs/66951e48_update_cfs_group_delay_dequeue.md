# sched/fair: Fix update_cfs_group() vs DELAY_DEQUEUE

- **Commit:** 66951e4860d3c688bfa550ea4a19635b57e00eca
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When DELAY_DEQUEUE keeps empty group entities in the runqueue to burn off lag, `update_cfs_group()` recalculates their weight based on the empty cgroup. This results in a very low weight being assigned to the delayed entity, causing it to not compete at the expected weight. The incorrectly low weight also inflates the entity's lag calculation, which when combined with `avg_vruntime()` using `scale_load_down()`, leads to scheduling artifacts and incorrect scheduling decisions.

## Root Cause

The `update_cfs_group()` function did not distinguish between normal dequeue operations (where empty group entities should be removed) and DELAY_DEQUEUE operations (where empty entities are intentionally retained). When an empty group entity is dequeued, `update_cfs_group()` computes its weight based on the current cgroup state, which for an empty group is nearly zero. This causes the delayed entity to be reweighted to an artificially low value, violating the assumption that it should compete at its original weight while burning off lag.

## Fix Summary

The fix adds a check in `update_cfs_group()` to skip weight recalculation for empty group entities (checking `!gcfs_rq->load.weight`). This preserves the original weight for delayed empty group entities, allowing them to compete at the expected weight and maintain correct lag calculations while they burn off their delayed dequeue lag.

## Triggering Conditions

This bug requires the DELAY_DEQUEUE feature and task group scheduling (CONFIG_FAIR_GROUP_SCHED). 
The triggering sequence involves: 1) A task group with positive lag that becomes empty when all tasks exit/migrate, 
2) DELAY_DEQUEUE keeping the empty group entity on the runqueue to burn off lag, 
3) `update_cfs_group()` being called on this empty group entity during subsequent scheduling operations.
The key condition is that `gcfs_rq->load.weight` becomes zero while the group entity remains queued, 
causing weight recalculation based on empty group state rather than preserving the original weight needed for proper lag burn-off.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs. In setup(), create a cgroup with custom weight using `kstep_cgroup_create()` and 
`kstep_cgroup_set_weight()`. Create 2+ tasks and add them to the cgroup via `kstep_cgroup_add_task()`. 
In run(), let tasks accumulate positive lag by running with different nice values to create weight imbalance, 
then use `kstep_task_pause()` to make all tasks in the cgroup exit/sleep, creating an empty group with lag. 
Use `kstep_tick_repeat()` to trigger scheduler operations that call `update_cfs_group()` on the empty entity.
Monitor via `on_tick_begin()` callback to check group entity weight and lag values. 
Bug manifests as group weight dropping to near-zero instead of preserving original weight, 
and inflated lag calculations affecting subsequent scheduling decisions.
