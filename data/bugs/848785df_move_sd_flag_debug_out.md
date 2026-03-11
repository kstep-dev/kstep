# sched/topology: Move sd_flag_debug out of #ifdef CONFIG_SYSCTL

- **Commit:** 848785df48835eefebe0c4eb5da7690690b0a8b7
- **Affected file(s):** kernel/sched/debug.c, kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

When CONFIG_SYSCTL is disabled but CONFIG_SCHED_DEBUG is enabled, the sd_flag_debug array definition is missing, causing compilation failures or undefined symbol errors. The debug code in topology.c tries to use sd_flag_debug for assertions on the sched_domain hierarchy, but the definition was incorrectly placed in a CONFIG_SYSCTL conditional block where it was only needed for the sysctl interface.

## Root Cause

A previous commit moved sd_flag_debug definition into an #ifdef CONFIG_SYSCTL region in debug.c. However, sd_flag_debug is required by CONFIG_SCHED_DEBUG code (not just CONFIG_SYSCTL). This created a conditional dependency mismatch: the symbol was only defined when CONFIG_SYSCTL was enabled, but the code using it was compiled when CONFIG_SCHED_DEBUG was enabled.

## Fix Summary

The fix moves the sd_flag_debug definition from debug.c (under CONFIG_SYSCTL) to topology.c (under CONFIG_SCHED_DEBUG). This ensures the array is defined whenever the debug code that uses it is compiled, resolving the missing symbol issue and properly aligning the conditional compilation with actual dependencies.

## Triggering Conditions

This is a build-time compilation issue triggered by specific kernel configuration:
- CONFIG_SCHED_DEBUG=y (enables scheduler domain assertions in topology.c)
- CONFIG_SYSCTL=n (disables sysctl interface, removing sd_flag_debug definition)
- CONFIG_SMP=y (required for topology.c compilation)
- Any code path in topology.c that references sd_flag_debug for domain flag validation
- The issue manifests as "undefined symbol sd_flag_debug" linker errors during kernel build
- No specific runtime scheduler state or CPU topology requirements - purely a conditional compilation mismatch

## Reproduce Strategy (kSTEP)

This is a compilation bug rather than a runtime scheduler behavior issue, so traditional kSTEP reproduction is not applicable. However, verification strategies include:
- Build test with CONFIG_SCHED_DEBUG=y, CONFIG_SYSCTL=n to trigger the original compilation failure
- Verify the fix by confirming sd_flag_debug is properly defined in topology.c under CONFIG_SCHED_DEBUG
- Test scheduler domain assertion code paths that would use sd_flag_debug during topology initialization
- Setup: Use kstep_topo_init() and kstep_topo_apply() to trigger domain construction
- Run: No specific runtime sequence needed - the fix prevents build failures, not runtime bugs
- Detection: Success is measured by successful kernel compilation and proper symbol resolution
