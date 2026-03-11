# sched/fair: Fix potential memory corruption in child_cfs_rq_on_list

- **Commit:** 3b4035ddbfc8e4521f85569998a7569668cccf51
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

The `child_cfs_rq_on_list` function uses `container_of` to convert a 'prev' pointer to a cfs_rq structure, but this pointer can originate from struct rq's `leaf_cfs_rq_list` head, making the conversion invalid and leading to memory corruption. Depending on the relative positions of `leaf_cfs_rq_list` and the task group pointer within the struct, this can cause a memory fault or access garbage data.

## Root Cause

The function fails to check whether the 'prev' pointer actually points to an embedded `leaf_cfs_rq_list` structure within a cfs_rq, or if it points directly to the list head itself. When `prev` equals `rq->leaf_cfs_rq_list`, the `container_of` operation converts a list head pointer using cfs_rq's field offset, accessing unrelated memory and potentially corrupting data.

## Fix Summary

The fix adds a check `if (prev == &rq->leaf_cfs_rq_list) return false;` before the `container_of` operation to ensure the pointer actually refers to a valid cfs_rq structure. This check is sufficient because only cfs_rqs on the same CPU are added to the list.

## Triggering Conditions

The bug triggers when `child_cfs_rq_on_list()` evaluates a `prev` pointer that points to `rq->leaf_cfs_rq_list` (the list head) instead of a valid cfs_rq structure. This occurs during CFS group scheduling operations when:
- Task groups with hierarchical relationships are created, causing cfs_rq structures to be added/removed from the leaf list
- The `rq->tmp_alone_branch` is set to `&rq->leaf_cfs_rq_list` during list manipulations in `list_add_leaf_cfs_rq()`
- A cfs_rq is not on the list (`cfs_rq->on_list == false`), causing the function to use `rq->tmp_alone_branch` as the `prev` pointer
- The `container_of()` operation then converts the list head pointer using cfs_rq's field offset, accessing invalid memory and potentially corrupting task group pointers

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create hierarchical task groups to trigger leaf_cfs_rq_list operations:
- In `setup()`: Use `kstep_cgroup_create()` to create parent and child task groups ("parent_tg", "child_tg")
- Set up CPU topology with `kstep_topo_init()` and `kstep_topo_apply()` for multi-CPU scheduling
- In `run()`: Create tasks with `kstep_task_create()` and assign to groups using `kstep_cgroup_add_task()`
- Pin tasks to different CPUs with `kstep_task_pin()` to force cross-CPU group operations
- Use `kstep_tick_repeat()` to advance scheduling and trigger list manipulations during group operations
- Monitor with `on_sched_group_alloc()` callback to observe when cfs_rq structures are created/modified
- Check for memory corruption by logging task group pointers before/after operations, or use memory access patterns that would crash if corruption occurs
