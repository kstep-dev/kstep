# sched: Fix build warning without CONFIG_SYSCTL

- **Commit:** 494dcdf46e5cdee926c9f441d37e3ea1db57d1da
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When CONFIG_SYSCTL is disabled, the compiler generates a warning that the function `sysctl_sched_uclamp_handler` is defined but not used. The sysctl-related functions and variables are only needed when the SYSCTL configuration option is enabled, but they were unconditionally compiled, leading to unused code and build warnings.

## Root Cause

The functions `sysctl_sched_uclamp_handler` and `uclamp_sync_util_min_rt_default`, along with the variables `sysctl_sched_uclamp_util_min` and `sysctl_sched_uclamp_util_max`, depend on CONFIG_SYSCTL being enabled. However, they were not properly wrapped with `#ifdef CONFIG_SYSCTL` guards, causing them to be compiled even when CONFIG_SYSCTL was disabled, resulting in unused code and compiler warnings.

## Fix Summary

The fix wraps all sysctl-related code with `#ifdef CONFIG_SYSCTL` guards, ensuring these functions and variables are only compiled when the SYSCTL feature is enabled. Additionally, the affected variables are marked with `__maybe_unused` to suppress warnings in configurations where they may not be used. This allows clean compilation across all kernel configurations.

## Triggering Conditions

This is a **build-time issue**, not a runtime scheduler bug. The "bug" occurs during kernel compilation when:
- CONFIG_SYSCTL is disabled in kernel configuration
- CONFIG_UCLAMP_TASK is enabled (utilization clamping feature)
- The compiler detects that `sysctl_sched_uclamp_handler()` and related functions are defined but never called
- Results in `-Wunused-function` compiler warnings during build
- Affects the uclamp (utilization clamping) subsystem in kernel/sched/core.c

## Reproduce Strategy (kSTEP)

This commit addresses a **compilation warning**, not a runtime scheduler behavior that can be reproduced with kSTEP. There is no scheduler malfunction to trigger at runtime. To "reproduce" this issue, one would need to:
- Configure kernel with `CONFIG_SYSCTL=n` and `CONFIG_UCLAMP_TASK=y`
- Compile the kernel and observe build warnings
- The fix ensures clean compilation across all kernel configurations
- No kSTEP driver is applicable since this is purely a build configuration issue
- No runtime testing or task scheduling behavior is involved
