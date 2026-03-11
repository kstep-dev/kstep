# Fix Sparse warnings due to undefined rt.c declarations

- **Commit:** 87514b2c24f294c32e9e743b095541dcf43928f7
- **Affected file(s):** kernel/sched/sched.h
- **Subsystem:** RT

## Bug Description

Three real-time scheduling group functions (unregister_rt_sched_group, free_rt_sched_group, alloc_rt_sched_group) are defined unconditionally in rt.c, but their declarations in sched.h were conditionally placed inside a CONFIG_CGROUP_SCHED block. This caused Sparse static analysis warnings reporting these symbols as undeclared when CONFIG_CGROUP_SCHED was disabled, even though the functions existed.

## Root Cause

The function declarations were incorrectly placed within the CONFIG_CGROUP_SCHED preprocessor conditional, but the corresponding definitions in rt.c exist unconditionally (both when CONFIG_CGROUP_SCHED is enabled and disabled). This mismatch between declaration visibility and definition visibility caused Sparse warnings and could lead to compilation issues or missed type checking.

## Fix Summary

The three RT scheduling group function declarations are moved outside the CONFIG_CGROUP_SCHED conditional block to match the unconditional nature of their definitions in rt.c. This ensures the declarations are always visible regardless of the CONFIG_CGROUP_SCHED setting, eliminating the Sparse warnings.

## Triggering Conditions

This bug is triggered during static analysis (Sparse) or compilation when CONFIG_CGROUP_SCHED is disabled in the kernel configuration. The three RT scheduling group functions (unregister_rt_sched_group, free_rt_sched_group, alloc_rt_sched_group) are defined unconditionally in kernel/sched/rt.c but their declarations in kernel/sched/sched.h were wrapped inside a CONFIG_CGROUP_SCHED conditional block. When CONFIG_CGROUP_SCHED is disabled, the declarations become invisible while the definitions remain, causing Sparse to report these symbols as undeclared. This is purely a static analysis issue and does not affect runtime scheduler behavior.

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced using the kSTEP runtime testing framework, as it is a static analysis issue rather than a runtime scheduler bug. The problem manifests during kernel compilation with Sparse static analysis when CONFIG_CGROUP_SCHED is disabled, not during kernel execution. To reproduce this issue, one would need to:
- Configure the kernel with CONFIG_CGROUP_SCHED=n 
- Build the kernel with Sparse static analysis enabled (make C=1 or C=2)
- Observe the Sparse warnings for the three RT scheduling group functions

The kSTEP framework is designed for testing runtime scheduler behavior and cannot detect compilation-time static analysis warnings.
