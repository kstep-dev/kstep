# sched: Fix performance regression introduced by mm_cid

- **Commit:** 223baf9d17f25e2608dbdff7232c095c1e612268
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

A previous commit introduced a per-memory-map concurrency ID (mm_cid) feature to track task memory contexts more efficiently. However, this implementation caused a significant performance regression in PostgreSQL sysbench workloads, measured by Aaron Lu. The regression occurred when context switching back and forth between threads belonging to different memory spaces in multi-threaded scenarios, due to excessive atomic operations being performed on each context switch.

## Root Cause

The initial mm_cid implementation freed and reallocated concurrency IDs immediately on every context switch, requiring frequent atomic operations to manage the cid bitmap. In workloads with many processes and threads frequently migrating between CPUs, this overhead becomes substantial and degrades overall performance.

## Fix Summary

The fix introduces per-mm/per-cpu current concurrency ID tracking that keeps allocated cids resident on their CPUs rather than freeing them immediately. It implements lazy reclamation of unused cid values through a periodic task-work mechanism, which delays clearing stale cids until they exceed a time threshold (SCHED_MM_CID_PERIOD_NS). This drastically reduces atomic operations during context switches while maintaining correct functionality through memory barriers and migration-aware cid transfer on task movement between CPUs.

## Triggering Conditions

The bug manifests in multi-threaded workloads with frequent context switches between threads belonging to different memory spaces (different processes). Key conditions:
- Multiple processes, each with multiple threads, running concurrently
- High frequency of context switches between threads from different memory maps
- Tasks migrating between CPUs, triggering mm_cid allocation/deallocation
- The original mm_cid implementation performs atomic operations on every context switch to manage the cid bitmap
- Heavy contention on the cid bitmap atomic operations becomes a bottleneck
- Performance degradation is measurable in CPU-intensive database workloads (PostgreSQL sysbench)

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved for driver). Create multiple processes with threads that frequently context switch:
- In setup(): Create 6-8 tasks representing threads from different processes using `kstep_task_create()`
- Use `kstep_task_pin()` to assign tasks to different CPU sets (CPUs 1-2 vs 3-4) initially
- In run(): Start all tasks with `kstep_task_wakeup()`, then simulate high context switch frequency with `kstep_tick_repeat(100)`
- Periodically change CPU affinity using `kstep_task_pin()` to force task migrations between CPUs
- Use `on_tick_begin()` callback to log context switch counts and measure scheduling overhead
- Monitor atomic operation frequency by tracking mm_cid allocations through kernel tracing
- Compare performance metrics (context switch latency, CPU utilization) between buggy and fixed kernels
- Success criteria: Significant reduction in atomic operations and improved context switch performance after fix
