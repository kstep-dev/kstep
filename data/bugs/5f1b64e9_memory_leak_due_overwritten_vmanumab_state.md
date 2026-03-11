# sched/numa: fix memory leak due to the overwritten vma->numab_state

- **Commit:** 5f1b64e9a9b7ee9cfd32c6b2fab796e29bfed075
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NUMA, core scheduler

## Bug Description

A memory leak occurs in the NUMA balancing code when multiple threads simultaneously access a shared VMA (Virtual Memory Area). The task_numa_work() function initializes vma->numab_state for each VMA, but when multiple threads observe that vma->numab_state is NULL at the same time, they all allocate memory and attempt to assign it, causing only one assignment to succeed while the others leak the allocated memory. This issue is reproducible on large multi-core servers with concurrent thread workloads like hackbench, resulting in hundreds of memory leak detections.

## Root Cause

The race condition occurs because vma->numab_state initialization is not atomic. Although the scheduler ensures that typically only one thread scans VMAs per numa_scan_period, another thread can enter during the next scan period before the previous allocation is complete. When two cores observe vma->numab_state is NULL simultaneously, both allocate memory, but only one assignment succeeds (the other is overwritten), causing the overwritten pointer to leak. The code lacked synchronization between the allocation and assignment steps.

## Fix Summary

The fix uses a cmpxchg (compare-and-swap) atomic operation to ensure only one thread successfully assigns vma->numab_state. Each thread allocates memory and then atomically swaps it into vma->numab_state only if it's still NULL; if another thread has already set it, the current thread frees its allocated pointer and continues. This eliminates the race condition and prevents memory leaks.

## Triggering Conditions

This bug occurs in the task_numa_work() function within the NUMA balancing subsystem when:
- Multiple threads share VMAs and concurrently trigger NUMA scanning
- Two or more threads simultaneously observe vma->numab_state as NULL during different numa_scan_periods
- Both threads allocate vma_numab_state structures and race to assign them to vma->numab_state
- One assignment succeeds while others are overwritten, causing memory leaks
- Requires threaded workloads (not process-based) and many concurrent threads (like hackbench with "thread" mode)
- Most reproducible on large multi-core servers (192+ cores) with high thread contention
- The race window exists between allocation and assignment in task_numa_work()

## Reproduce Strategy (kSTEP)

Use 4+ CPUs to create thread contention in NUMA scanning. In setup(), create multiple kthreads that will share memory regions and trigger concurrent task_numa_work() execution. Use kstep_kthread_create() to create threads that simulate the hackbench workload pattern. In run(), use kstep_kthread_start() to begin concurrent execution and kstep_tick_repeat() to advance through multiple numa_scan_periods. Monitor vma->numab_state allocation attempts through on_tick_begin() callbacks and trace allocations. Detect the bug by instrumenting task_numa_work() to log when multiple threads attempt vma->numab_state allocation for the same VMA simultaneously. Check for memory leaks by counting allocations vs successful assignments - excess allocations indicate the overwrite condition occurred.
