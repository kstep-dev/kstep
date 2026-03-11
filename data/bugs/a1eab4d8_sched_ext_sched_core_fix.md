# sched_ext, sched/core: Fix build failure when !FAIR_GROUP_SCHED && EXT_GROUP_SCHED

- **Commit:** a1eab4d813f7b6e606ed21381b8cfda5c59a87e5
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** EXT (sched_ext, sched/core)

## Bug Description

The kernel fails to build when CONFIG_FAIR_GROUP_SCHED is disabled but CONFIG_EXT_GROUP_SCHED is enabled. The build failure occurs in the `tg_weight()` function in kernel/sched/core.c, which attempts to access a field that no longer exists at the expected location due to a prior refactoring.

## Root Cause

A previous commit (6e6558a6bc41) refactored scheduler code by moving SCX-related fields from the task_group struct into a nested scx_task_group struct. However, the refactoring missed updating one usage site: the `tg_weight()` function in the `!CONFIG_FAIR_GROUP_SCHED` branch still referenced the old field path `tg->scx_weight`, which no longer exists after the fields were relocated into the `tg->scx` sub-struct.

## Fix Summary

The fix updates the field reference in `tg_weight()` from `tg->scx_weight` to `tg->scx.weight`, ensuring it correctly accesses the weight field through the new nested scx_task_group struct. This single-line change resolves the build failure for the affected kernel configuration.

## Triggering Conditions

This is a **build-time failure**, not a runtime bug. The conditions required to trigger the compilation failure are:
- Kernel configuration with `CONFIG_FAIR_GROUP_SCHED=n` (CFS group scheduling disabled)
- Kernel configuration with `CONFIG_EXT_GROUP_SCHED=y` (sched_ext group scheduling enabled)  
- Compilation of kernel/sched/core.c reaches the `tg_weight()` function's `!CONFIG_FAIR_GROUP_SCHED` branch
- The code path attempts to access the obsolete `tg->scx_weight` field reference
- Compiler error occurs due to the missing field in the task_group struct after refactoring

## Reproduce Strategy (kSTEP)

Since this is a compilation failure rather than a runtime behavioral bug, kSTEP cannot directly reproduce the issue at runtime. However, to verify the fix works correctly in the affected code path:
- Use 2+ CPUs (CPU 0 reserved for driver)
- In setup(): Create task groups with `kstep_cgroup_create()` and set weights with `kstep_cgroup_set_weight()`
- In run(): Create tasks and assign them to different cgroups to exercise the group scheduling code paths
- Use `kstep_task_create()` and `kstep_cgroup_add_task()` to populate the cgroups with tasks
- Call `kstep_tick_repeat()` to trigger scheduler activity that might invoke weight-related functions
- The driver would only verify that the kernel boots and runs correctly with the fix applied
- No specific runtime failure pattern to detect - the bug was purely a missing field reference
