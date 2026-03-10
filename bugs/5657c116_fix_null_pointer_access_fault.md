# sched/core: Fix NULL pointer access fault in sched_setaffinity() with non-SMP configs

- **Commit:** 5657c116783545fb49cd7004994c187128552b12
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

Calling `sched_setaffinity()` on a uniprocessor kernel built with non-SMP config causes a NULL pointer dereference leading to a general protection fault. The bug occurs because `alloc_user_cpus_ptr()` returns NULL in non-SMP configurations, but the subsequent code unconditionally calls `cpumask_copy()` with this NULL pointer, resulting in a crash.

## Root Cause

In non-SMP configurations, the `user_cpus_ptr` field is not used, so `alloc_user_cpus_ptr()` returns NULL. The original code checked for memory allocation failure only when `CONFIG_SMP` was enabled, but then unconditionally executed `cpumask_copy(user_mask, in_mask)` regardless of the CONFIG_SMP setting or whether `user_mask` was NULL. This causes a NULL pointer dereference in non-SMP kernels.

## Fix Summary

The fix reorganizes the logic to first check if `user_mask` is not NULL before calling `cpumask_copy()`. If `user_mask` is NULL, it only treats it as a fatal error if `CONFIG_SMP` is enabled. In non-SMP configurations where NULL is expected, execution continues without error.

## Triggering Conditions

This bug only manifests in uniprocessor kernels built with non-SMP configuration (`CONFIG_SMP=n`). The triggering sequence requires:
- A non-SMP kernel where `alloc_user_cpus_ptr()` always returns NULL since user_cpus_ptr is unused
- Any call to `sched_setaffinity()` syscall on any valid process 
- The syscall path reaches the point where `cpumask_copy(user_mask, in_mask)` is called with NULL user_mask
- No specific task states, CPU topology, or timing conditions are required - just the syscall invocation
- The bug occurs deterministically on first `sched_setaffinity()` call in non-SMP kernels

## Reproduce Strategy (kSTEP)

This bug cannot be directly reproduced in kSTEP since kSTEP requires SMP systems (CPU 0 reserved for driver), but the conceptual approach would be:
- **CPU Requirements**: Not applicable - bug only occurs in non-SMP configurations
- **Setup**: Create a single task using `kstep_task_create()` to have a target for setaffinity
- **Triggering**: The bug would require a direct syscall to `sched_setaffinity()`, which kSTEP doesn't expose
- **Alternative Strategy**: Modify kSTEP's task management to simulate NULL user_mask condition by:
  - Adding a test hook in `sched_setaffinity()` to force `alloc_user_cpus_ptr()` to return NULL
  - Use `kstep_task_pin()` which internally may call setaffinity-related code paths
- **Detection**: Monitor for general protection fault or NULL pointer dereference in kernel logs
- **Note**: This bug is configuration-specific and cannot be reproduced on standard SMP kSTEP setups
