# sched_ext: Fix the memleak for sch->helper objects

- **Commit:** 517a44d18537ef8ab888f71197c80116c14cee0a
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

When sched_ext functionality is disabled or unloaded, the `sch->helper` kthread worker object allocated during initialization leaks 128 bytes of memory. This occurs because the cleanup code only stops the kthread task but fails to properly destroy and free the underlying worker structure, causing memory to accumulate each time the scheduler is enabled/disabled.

## Root Cause

The original code used `kthread_stop(sch->helper->task)` to cleanup, which only terminates the kthread task itself but doesn't deallocate the kthread_worker structure that wraps it. The worker structure was allocated by `kthread_create_worker_on_node()` during initialization but had no corresponding deallocation, resulting in a memory leak that kmemleak detected.

## Fix Summary

Replace `kthread_stop(sch->helper->task)` with `kthread_destroy_worker(sch->helper)` in both the normal cleanup path (`scx_sched_free_rcu_work()`) and the error handling path (`scx_alloc_and_add_sched()`). The `kthread_destroy_worker()` function properly stops the worker thread and frees the worker structure, eliminating the memory leak.

## Triggering Conditions

The bug triggers when sched_ext is enabled and subsequently disabled/unloaded. Specifically:
- A BPF sched_ext scheduler program must be loaded via `bpf_struct_ops_link_create()` syscall
- During enable, `scx_enable()` calls `kthread_create_worker_on_node()` to allocate the helper worker (128 bytes)
- The bug occurs during cleanup when the scheduler is disabled (normal shutdown or error path)
- In `scx_sched_free_rcu_work()`, `kthread_stop()` only terminates the task but leaks the worker structure
- Each enable/disable cycle accumulates 128 bytes of leaked memory in the kthread_worker allocation
- The leak is detectable by kmemleak after multiple sched_ext enable/disable cycles

## Reproduce Strategy (kSTEP)

This bug requires BPF sched_ext program loading/unloading, which is outside kSTEP's current scope (focused on CFS scheduling). A theoretical kSTEP approach would need:
- Use `kstep_write()` to interact with sysfs interfaces for sched_ext if available
- Alternatively, implement system calls via inline assembly to load minimal BPF sched_ext programs
- Create multiple enable/disable cycles: load BPF program → enable sched_ext → disable sched_ext → repeat
- Use `on_tick_begin()` callback to periodically check memory allocation counters or kmalloc stats
- Monitor `/proc/slabinfo` for kthread_worker allocations or implement custom memory tracking
- Detect the bug by observing increasing memory usage with no corresponding deallocations
- Requires kernel built with CONFIG_SCHED_EXT=y and kmemleak debugging enabled
- Note: Current kSTEP framework lacks BPF loading capabilities, requiring significant extensions
