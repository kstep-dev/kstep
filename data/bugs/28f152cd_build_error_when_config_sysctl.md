# sched/rt: fix build error when CONFIG_SYSCTL is disable

- **Commit:** 28f152cd0926596e69d412467b11b6fe6fe4e864
- **Affected file(s):** kernel/sched/rt.c
- **Subsystem:** RT

## Bug Description

Build fails with compilation errors when CONFIG_SYSCTL is disabled. The compiler treats unused function warnings as errors, reporting that `sched_rr_handler` and `sched_rt_handler` are defined but not used. This prevents kernel builds with `-Werror=unused-function` when CONFIG_SYSCTL is not selected, breaking configurations that don't need sysctl support.

## Root Cause

Functions and variables related to sysctl configuration (`sched_rr_handler`, `sched_rt_handler`, `sysctl_sched_rr_timeslice`, `sched_rt_global_constraints`, and `sched_rt_global_validate`) were not guarded by `#ifdef CONFIG_SYSCTL` preprocessor directives. When CONFIG_SYSCTL is disabled, these symbols are still compiled as unused code, triggering unused-function compiler errors in strict build environments.

## Fix Summary

Wrap all sysctl-related code in proper `#ifdef CONFIG_SYSCTL` guards. This ensures that handler functions, validation functions, and sysctl variables are only compiled when sysctl support is enabled, eliminating the unused-function warnings when CONFIG_SYSCTL is disabled.

## Triggering Conditions

This is a build-time compilation error, not a runtime scheduler bug. The triggering conditions are:
- Kernel configuration has `CONFIG_SYSCTL=n` (sysctl support disabled)
- Compiler flags include `-Werror=unused-function` (treating unused function warnings as errors)
- Functions `sched_rr_handler` and `sched_rt_handler` in `kernel/sched/rt.c` are compiled but unreferenced
- The `sysctl_sched_rr_timeslice` variable and related sysctl infrastructure become unused
- Affects any kernel build system using strict warning-as-error compilation policies

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced using kSTEP because it is a build-time compilation error, not a runtime scheduler behavior issue. kSTEP is designed to test scheduler runtime behavior and state transitions, but this bug manifests during the compilation phase when certain kernel configuration options are disabled.

To reproduce this bug, one would need to:
- Configure a kernel with `CONFIG_SYSCTL=n`
- Compile with `-Werror=unused-function` 
- Observe the compilation failure in `kernel/sched/rt.c`

kSTEP testing would be applicable after the fix to verify that RT scheduler functionality works correctly even when sysctl support is disabled.
