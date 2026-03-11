# sched_ext: Merge branch 'for-6.16-fixes' into for-6.17

- **Commit:** e4e149dd2f80b3f61d738f0b7d9cc9772c1353a4
- **Affected file(s):** kernel/sched/core.c, kernel/sched/ext.c, kernel/sched/ext.h
- **Subsystem:** sched_ext (scheduler extension), CFS group scheduling

## Bug Description

The bug is that `scx_group_set_weight()` was being called prematurely during task group creation in `sched_create_group()`, before the scheduler extension subsystem had properly initialized the task group. This could lead to incorrect weight initialization for extended scheduler classes and cause issues with CPU bandwidth control interface implementation.

## Root Cause

The original code directly initialized `root_task_group.scx_weight` to `CGROUP_WEIGHT_DFL` in `sched_init()` without going through proper initialization, and `scx_group_set_weight()` was being invoked at group creation time before the necessary setup was complete. The scheduler extension framework requires a dedicated initialization function to properly set up task group state.

## Fix Summary

The fix introduces a new `scx_tg_init()` function that properly initializes scheduler extension task group state at the correct time. Instead of directly assigning the weight during `sched_init()`, the code now calls `scx_tg_init()` which handles initialization through the proper code path. This ensures that weight initialization happens after all necessary setup is complete and prevents premature calls to `scx_group_set_weight()` from `sched_create_group()`.

## Triggering Conditions

The bug occurs when creating new task groups (cgroups) while a sched_ext scheduler with cgroup operations is loaded. Specifically:
- CONFIG_EXT_GROUP_SCHED must be enabled 
- A sched_ext scheduler with `ops.cgroup_set_weight()` callback must be active
- Task group creation triggered via cgroup creation calls `sched_create_group()`
- The premature `scx_group_set_weight()` call happens before `ops.cgroup_init()` 
- This results in `ops.cgroup_set_weight()` being invoked with an uninitialized cgroup context
- The timing issue occurs during the task group allocation and initialization sequence

## Reproduce Strategy (kSTEP)

This bug requires a loaded sched_ext scheduler with cgroup ops, which kSTEP cannot directly control. However, the initialization ordering issue can be observed:
- Use at least 2 CPUs (CPU 0 reserved for driver)
- In `setup()`: Load a minimal sched_ext scheduler if possible or observe existing initialization
- In `run()`: Create cgroups to trigger `sched_create_group()` using `kstep_cgroup_create("test_group")`
- Add callback logging in scheduler extension initialization paths if accessible
- Use `on_sched_group_alloc()` callback to observe task group creation events
- Check task group state and weight initialization ordering via kernel logging
- Detect the bug by observing incorrect weight callback invocation before proper initialization
- Compare behavior between buggy and fixed kernels during cgroup creation sequences
