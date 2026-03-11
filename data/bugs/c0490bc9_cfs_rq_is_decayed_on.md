# sched/fair: Fix cfs_rq_is_decayed() on !SMP

- **Commit:** c0490bc9bb62d9376f3dd4ec28e03ca0fef97152
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

On non-SMP systems, the `cfs_rq_is_decayed()` function was unconditionally returning `true`, which caused the per-queue `leaf_cfs_rq_list` to not be maintained correctly. This resulted in warnings from `assert_list_leaf_cfs_rq()` because the debug interface relies on this list to print per-cfs_rq stats, even on !SMP.

## Root Cause

A previous commit (0a00a354644e) changed the logic of how `leaf_cfs_rq_list` is managed, but the !SMP version of `cfs_rq_is_decayed()` was not updated accordingly. The !SMP version was a placeholder that always returned `true`, which prevented proper maintenance of the `leaf_cfs_rq_list` structure that the scheduler debug interface depends on.

## Fix Summary

Changed the !SMP version of `cfs_rq_is_decayed()` to properly check `!cfs_rq->nr_running` instead of always returning `true`. This ensures the `leaf_cfs_rq_list` is maintained correctly on non-SMP systems, matching the semantic behavior expected by the scheduler's list management code.

## Triggering Conditions

This bug occurs specifically on !SMP (single-CPU) systems when the scheduler debug interface is accessed. The issue manifests when:
- Running on a !SMP kernel configuration
- Tasks are created and destroyed, causing changes to `cfs_rq->nr_running`
- The `leaf_cfs_rq_list` maintenance logic in `list_add_leaf_cfs_rq()` and `list_del_leaf_cfs_rq()` is triggered
- Debug interfaces attempt to traverse the `leaf_cfs_rq_list` (e.g., through `/proc/sched_debug`)
- The incorrect `cfs_rq_is_decayed()` logic causes list inconsistencies, triggering warnings in `assert_list_leaf_cfs_rq()`

The bug is not a crash but rather a list maintenance inconsistency that shows up as warnings from debug assertions when the scheduler's internal data structures are checked.

## Reproduce Strategy (kSTEP)

**Note**: This bug is !SMP specific, but kSTEP runs on SMP systems. However, we can test the equivalent logic by:
- **CPUs needed**: Single CPU (CPU 1, as CPU 0 is reserved for driver)
- **Setup**: Create multiple tasks and task groups to exercise `leaf_cfs_rq_list` management
- **Reproduction steps**:
  1. Use `kstep_cgroup_create()` to create task groups (triggers cfs_rq allocation)
  2. Use `kstep_task_create()` and `kstep_cgroup_add_task()` to populate groups
  3. Use `kstep_task_wakeup()` and `kstep_task_pause()` to change `nr_running` values
  4. Call `kstep_print_sched_debug()` to trigger debug interface traversal of `leaf_cfs_rq_list`
- **Detection**: In `on_sched_group_alloc()` callback, verify `cfs_rq_is_decayed()` returns correct value
- **Key observation**: Monitor that `cfs_rq_is_decayed()` properly reflects `!cfs_rq->nr_running` state rather than always returning `true`
