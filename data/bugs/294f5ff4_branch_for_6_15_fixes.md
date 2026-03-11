# sched_ext: Merge branch 'for-6.15-fixes' into for-6.16

- **Commit:** 294f5ff47405f920d85b8d411ddbc52ed708a423
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The `exit_dump` buffer in `scx_exit_info` could fail to allocate when `exit_dump_len` is large (exceeds single-page limits), causing scheduler exit handling to fail. The code uses `kzalloc` which only allocates from the kernel's page allocator and fails silently for large allocations, leaving the exit information incomplete during critical scheduler shutdown scenarios.

## Root Cause

The code incorrectly uses `kzalloc(exit_dump_len, GFP_KERNEL)` for a variable-sized allocation that may exceed PAGE_SIZE. The `kzalloc` function is designed for small, fixed-size allocations and cannot fall back to `vmalloc` for larger sizes. This causes allocation failures when the exit dump size is large, impacting scheduler debugging and exit reporting.

## Fix Summary

Change the allocation functions to use `kvzalloc` (for allocation) and `kvfree` (for deallocation) instead of `kzalloc` and `kfree`. The `kvzalloc` API automatically uses `vmalloc` for allocations that exceed page limits, ensuring reliable allocation of variable-sized buffers regardless of size.

## Triggering Conditions

The bug occurs in the sched_ext exit path when a BPF scheduler triggers an error or failure. Specifically:
- A sched_ext BPF scheduler must be loaded and active
- The scheduler must encounter an error condition that triggers `scx_ops_error()` (e.g., watchdog timeout, BPF program fault, invalid operation)  
- The `exit_dump_len` parameter passed to `alloc_exit_info()` must exceed PAGE_SIZE (typically 4KB)
- The system must be under memory pressure where contiguous physical memory allocation fails
- The failure occurs when `kzalloc(exit_dump_len, GFP_KERNEL)` returns NULL for large allocations, preventing proper error reporting and scheduler exit cleanup

## Reproduce Strategy (kSTEP)

Create a kSTEP driver that loads a minimal sched_ext BPF scheduler, forces large exit dump allocation, and triggers memory pressure:
- **CPUs needed**: 2+ (CPU 0 reserved for driver)
- **Setup**: Use `kstep_cgroup_create()` to create test cgroups, `kstep_task_create()` for multiple tasks
- **Memory pressure**: Use `kstep_task_fork()` to create many tasks consuming memory, reducing available contiguous pages
- **Exit dump trigger**: Implement a custom BPF scheduler or trigger watchdog timeout by blocking in BPF program
- **Force large dump**: Configure sched_ext to generate large debug dumps (many runqueues, high task counts)  
- **Detection**: Monitor allocation failures in exit path via callback hooks or kernel logs, check for incomplete exit_info structures
- **Validation**: Compare behavior before/after kvzalloc fix - old kernel fails silently, new kernel succeeds with vmalloc fallback
