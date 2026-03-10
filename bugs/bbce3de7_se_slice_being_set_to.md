# sched/eevdf: Fix se->slice being set to U64_MAX and resulting crash

- **Commit:** bbce3de72be56e4b5f68924b7da9630cc89aa1a8
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS/EEVDF

## Bug Description

When `dequeue_entities()` dequeues a delayed group entity followed by its parent, and the parent's dequeue is delayed, the slice variable can be set to U64_MAX. This causes cascading calculation errors in subsequent scheduler operations, ultimately leading to corrupted vruntime values and NULL pointer dereferences in `pick_eevdf()`, resulting in kernel crashes observed in production.

## Root Cause

The slice variable is unconditionally initialized to `cfs_rq_min_slice()` of a potentially empty cfs_rq before the main dequeue loop. When the loop breaks due to a delayed parent dequeue without updating slice, the U64_MAX value (returned for empty queues) is propagated to the parent entity. This poisoned slice value then causes overflow in vlag calculations and vruntime adjustments, breaking the scheduler's eligibility checks.

## Fix Summary

Move the slice assignment from before the first `for_each_sched_entity()` loop to inside the loop, executed only after successfully dequeuing an entity. This ensures slice is always derived from a non-empty cfs_rq, preventing U64_MAX from being used in subsequent scheduler calculations.

## Triggering Conditions

This bug requires a cgroup hierarchy where a delayed group entity is dequeued, followed by its parent's dequeue being delayed. The specific sequence is: (1) A group entity becomes empty (no queued tasks) and delayed, making `cfs_rq_min_slice()` return U64_MAX, (2) `dequeue_entities()` is called on this empty group entity, (3) The entity's parent also needs delayed dequeue, causing the first loop to break without updating slice, (4) The second loop propagates the U64_MAX slice to parent entity, (5) Subsequent scheduler operations like `update_entity_lag()`, `place_entity()`, and `pick_eevdf()` use this corrupted slice value, leading to overflow in vlag/vruntime calculations and potential NULL pointer dereference.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved). In `setup()`: create cgroup hierarchy using `kstep_cgroup_create("parent")` and `kstep_cgroup_create("parent/child")`. In `run()`: create task with `kstep_task_create()`, add to child cgroup via `kstep_cgroup_add_task("parent/child", task->pid)`, then pause/remove task to empty the child cgroup. Create scenarios where parent also becomes empty/delayed during dequeue operations. Use `on_tick_begin()` to monitor for entities with slice values approaching U64_MAX. Use `kstep_tick_repeat()` to advance scheduling decisions and trigger dequeue paths. Monitor for vruntime corruption (huge positive/negative values), vlag anomalies, and scheduler state via custom logging. The bug manifests as corrupted scheduler state leading to pick_eevdf() returning NULL and subsequent crashes during task selection.
