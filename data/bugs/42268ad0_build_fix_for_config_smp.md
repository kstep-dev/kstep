# sched_ext: Build fix for !CONFIG_SMP

- **Commit:** 42268ad0eb4142245ea40ab01a5690a40e9c3b41
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

On non-SMP (uniprocessor) kernel configurations, the code fails to compile because `scx_dispatch_from_dsq()` calls `move_remote_task_to_local_dsq()`, but this function is only defined under `#ifdef CONFIG_SMP`. This causes undefined reference build failures on UP systems.

## Root Cause

The function `move_remote_task_to_local_dsq()` was only defined in the `#ifdef CONFIG_SMP` block of the code, but the caller `scx_dispatch_from_dsq()` in the `#else` block (for !CONFIG_SMP) still invokes this function. This creates a missing function symbol for UP configurations.

## Fix Summary

Add a dummy inline implementation of `move_remote_task_to_local_dsq()` in the `#else CONFIG_SMP` block that simply triggers a warning via `WARN_ON_ONCE(1)`. This allows the code to compile on UP systems while alerting developers if the function is unexpectedly called.

## Triggering Conditions

This is a compile-time bug, not a runtime bug. The conditions to trigger it are:
- Kernel built with `CONFIG_SCX=y` to enable sched_ext support
- Kernel built with `CONFIG_SMP=n` (uniprocessor configuration)
- Building targets that include `kernel/sched/ext.c`
- The `scx_dispatch_from_dsq()` function path that calls `move_remote_task_to_local_dsq()`
- Missing function definition causes undefined reference link error

The bug manifests during compilation/linking, not during execution. On UP systems, `move_remote_task_to_local_dsq()` was declared but not defined.

## Reproduce Strategy (kSTEP)

This build-time bug cannot be reproduced using kSTEP since it occurs during compilation, not runtime. kSTEP operates on already-compiled kernels and cannot simulate compile-time configuration issues.

However, if attempting to test the fix on a UP kernel:
- Setup: Single CPU system (but kSTEP requires minimum 2 CPUs)
- The fix adds a dummy function that triggers `WARN_ON_ONCE(1)` if called
- On proper UP systems, the sched_ext dispatch paths should never invoke remote task movement
- Any call to the dummy function would indicate a logic error in sched_ext UP handling
- Monitor dmesg for the warning if the function is incorrectly called

Note: This bug is purely a build issue and requires kernel reconfiguration with `CONFIG_SMP=n` to reproduce the original problem.
