# sched/autogroup: Fix sysctl move

- **Commit:** 82f586f923e3ac6062bc7867717a7f8afc09e0ff
- **Affected file(s):** kernel/sched/autogroup.c
- **Subsystem:** autogroup (core scheduler)

## Bug Description

The sysctl interface for `/proc/sys/kernel/sched_autogroup_enabled` became inaccessible after a previous refactoring moved autogroup sysctls into their own file. Additionally, using the `noautogroup` kernel command-line parameter would trigger a boot error. The sysctl registration function was incorrectly placed in a conditional path that only executes when the `noautogroup` parameter is specified, causing the sysctl table to never register during normal boot.

## Root Cause

The initialization function `sched_autogroup_sysctl_init()` was placed in the `setup_autogroup()` function, which is invoked via `__setup("noautogroup", ...)` only when the `noautogroup` kernel parameter is provided. This means:
- In normal boot (without `noautogroup`), the sysctl table is never registered, leaving `/proc/sys/kernel/sched_autogroup_enabled` inaccessible
- When `noautogroup` is used, the sysctl is belatedly registered after the feature is already disabled

## Fix Summary

Move the `sched_autogroup_sysctl_init()` call from the `setup_autogroup()` function to the `autogroup_init()` function, which always executes during kernel initialization. This ensures the sysctl table is registered unconditionally, regardless of kernel command-line parameters.

## Triggering Conditions

This bug manifests during kernel boot when autogroup is enabled (default configuration) without the `noautogroup` kernel parameter. The sysctl registration function `sched_autogroup_sysctl_init()` is only called from `setup_autogroup()`, which is bound to the `noautogroup` command-line parameter via `__setup()`. In normal boot scenarios:
- `autogroup_init()` executes but doesn't register sysctls
- `/proc/sys/kernel/sched_autogroup_enabled` becomes inaccessible 
- No runtime conditions or task states are required - the bug occurs during kernel initialization
- The bug is deterministic and affects all normal boot sequences without `noautogroup`

## Reproduce Strategy (kSTEP)

This configuration bug occurs during kernel boot and affects sysctl accessibility rather than runtime scheduler behavior. To reproduce with kSTEP:
- Use 2+ CPUs (CPU 0 reserved for driver)
- In `setup()`: Check sysctl accessibility using `kstep_write()` or file operations
- Attempt to read/write `/proc/sys/kernel/sched_autogroup_enabled` 
- The bug manifests as missing sysctl entries or access errors
- Use `on_tick_begin()` callback to log sysctl accessibility during early execution
- Detection: Check if `kstep_write("/proc/sys/kernel/sched_autogroup_enabled", "1", 1)` fails
- Compare behavior between buggy kernel (inaccessible) vs fixed kernel (accessible)
- No specific task creation or scheduling operations needed - focus on sysctl interface testing
