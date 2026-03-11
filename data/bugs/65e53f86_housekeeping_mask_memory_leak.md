# sched/isolation: Fix housekeeping_mask memory leak

- **Commit:** 65e53f869e9f92a23593c66214b88e54fb190a13
- **Affected file(s):** kernel/sched/isolation.c
- **Subsystem:** Isolation

## Bug Description

When `nohz_full=` or `isolcpus=nohz` kernel parameters are used on a kernel built without CONFIG_NO_HZ_FULL support, the `housekeeping_mask` cpumask is allocated but never freed. This occurs because the CONFIG_NO_HZ_FULL check happens after memory allocations, causing an early return without freeing the already-allocated mask, resulting in a boot-time memory leak.

## Root Cause

The CONFIG_NO_HZ_FULL availability check was performed after allocating `housekeeping_mask` in the initial setup. When CONFIG_NO_HZ_FULL was disabled, the code would `goto free_housekeeping_staging` without freeing `housekeeping_mask`, since it was allocated only when `!housekeeping_flags` (first call), but the cleanup path did not account for this allocation.

## Fix Summary

The fix moves the CONFIG_NO_HZ_FULL check to the beginning of `housekeeping_setup()`, before any memory allocations occur. This prevents unnecessary allocations and avoids the memory leak by returning early if the configuration is unsupported, ensuring no resources are allocated that won't be used.

## Triggering Conditions

This bug occurs during kernel boot-time parameter parsing in the isolation subsystem:
- Kernel must be built without CONFIG_NO_HZ_FULL support
- Boot parameters must include `nohz_full=` or `isolcpus=nohz` with CPU specifications
- The `housekeeping_setup()` function is called for the first time (`!housekeeping_flags` condition)
- Memory allocation via `alloc_bootmem_cpumask_var(&non_housekeeping_mask)` succeeds
- CONFIG_NO_HZ_FULL check fails, causing early return without freeing allocated `housekeeping_mask`
- If `housekeeping_flags` was previously zero, `housekeeping_mask` gets allocated but cleanup path doesn't free it

## Reproduce Strategy (kSTEP)

This bug cannot be directly reproduced using kSTEP since it occurs during boot-time kernel parameter parsing, before the scheduler runtime that kSTEP operates on. The memory leak happens in `housekeeping_setup()` during early kernel initialization.

To observe related behavior, a kSTEP driver could:
- Use 2+ CPUs (CPU 0 reserved for driver)
- Call `kstep_write("/proc/sys/kernel/nohz", "0", 1)` to check nohz configuration
- Monitor housekeeping-related scheduler behavior with `on_tick_begin()` callback
- Verify CPU isolation behavior via `kstep_task_pin()` and observe scheduling patterns
- Log any housekeeping-related warnings in kernel messages
- Detection would rely on observing inconsistent isolation behavior or kernel warnings rather than the memory leak itself, which occurs before kSTEP execution begins
