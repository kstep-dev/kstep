# sched/debug: Fix memory corruption caused by multiple small reads of flags

- **Commit:** 8d4d9c7b4333abccb3bf310d76ef7ea2edb9828f
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core

## Bug Description

Reading `/proc/sys/kernel/sched_domain/cpu*/domain0/flags` multiple times with small reads triggers memory corruption (oopses with SLUB corruption issues). The bug occurs because the `kfree()` call attempts to free a pointer that has been offset from the original allocation point, causing the heap allocator to attempt to free from an invalid address.

## Root Cause

In the `sd_ctl_doflags()` function, the pointer `tmp` is allocated via `kcalloc()` to hold the formatted flag names. The code then performs pointer arithmetic (`tmp += *ppos`) to skip past already-read data when handling partial reads. Subsequently, `kfree(tmp)` is called on this offset pointer, rather than on the original allocation point. This causes heap corruption because `kfree()` expects the exact pointer returned by the allocator.

## Fix Summary

The fix introduces a separate pointer `buf` to track the original allocation, while `tmp` is used for pointer arithmetic and data copying. The `kfree()` call now frees the original `buf` pointer, preventing heap corruption on partial reads.

## Triggering Conditions

The bug requires accessing `/proc/sys/kernel/sched_domain/cpu*/domain0/flags` with multiple partial reads. Specifically:
- The sched domain must have multiple flags set (to create a buffer with sufficient size)
- First read with a `ppos` offset of 0 (full or partial read) 
- Second read with a non-zero `ppos` offset (continuing from previous read position)
- The `sd_ctl_doflags()` function performs `tmp += *ppos` pointer arithmetic on the allocated buffer
- When `kfree(tmp)` is called, it frees the offset pointer rather than the original allocation
- This triggers SLUB corruption detection as the heap allocator receives an invalid pointer

## Reproduce Strategy (kSTEP)

The bug is in the procfs handler, not the scheduler core, so reproduction requires simulating procfs access:
1. Use at least 2 CPUs (CPU 0 reserved for driver, use CPU 1+)
2. In `setup()`: Create a basic topology with `kstep_topo_init()` and `kstep_topo_apply()`
3. In `run()`: Use `kstep_write()` to directly access `/proc/sys/kernel/sched_domain/cpu1/domain0/flags`
4. Perform multiple small reads by opening the file, reading a few bytes, closing, then reopening and reading more
5. Use `kstep_write()` or direct file operations to simulate the partial read behavior
6. Monitor for kernel oops/SLUB corruption messages in the kernel logs
7. The bug manifests as memory corruption, detectable through SLUB debug output
8. Success means triggering a kernel oops due to `kfree()` on an offset pointer
