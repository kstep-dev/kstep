# sched_ext: Avoid live-locking bypass mode switching

- **Commit:** 62dcbab8b0ef21729532600039fd514c09407092
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

A poorly behaving BPF scheduler can live-lock the system by repeatedly accessing the same Dispatch Queue (DSQ) on a large NUMA system. When switching to bypass mode, the kernel must dequeue and re-enqueue all runnable tasks; if the DSQs are being continuously contended by the misbehaving scheduler, this operation can take tens of seconds, leading to soft lockups and cascading failures. This issue was observed on 2x Intel Sapphire Rapids machines with 224 logical CPUs.

## Root Cause

The retry loops in `consume_dispatch_q()` and `scx_dispatch_from_dsq()` repeatedly race against `scx_ops_bypass()` attempting to dequeue tasks from DSQs. On large multi-socket systems with many CPUs, continuous contention causes these loops to spin without making progress while the bypass mode transition holds locks, preventing the system from making forward progress.

## Fix Summary

The fix introduces `scx_ops_breather()` function that injects artificial delays when bypass mode switching is in progress (tracked via `scx_ops_breather_depth` atomic counter). The breather is called in the retry loops of `consume_dispatch_q()` and `scx_dispatch_from_dsq()` to yield the runqueue lock and CPU cycles, allowing the bypass mode transition to complete in a timely manner without live-locking.

## Triggering Conditions

The bug requires a sched_ext BPF scheduler that continuously contends the same Dispatch Queue (DSQ) on a large NUMA system (observed on 224+ logical CPUs). The live-locking occurs when:
- A misbehaving BPF scheduler repeatedly accesses/bangs on the same DSQ across multiple CPUs
- Bypass mode switching is triggered (e.g., scheduler error, manual disable)
- The retry loops in `consume_dispatch_q()` and `scx_dispatch_from_dsq()` race against `scx_ops_bypass()`
- Multiple CPUs attempt to dequeue/re-enqueue tasks from the contended DSQ simultaneously
- The contention prevents forward progress in bypass mode transition, causing soft lockups
- Large CPU count amplifies the contention as more CPUs compete for the same DSQ resources

## Reproduce Strategy (kSTEP)

This bug targets sched_ext functionality which is not directly supported by the current kSTEP framework. A theoretical reproduction approach would require:
- Setup with 8+ CPUs (CPU 0 reserved for driver) to simulate multi-CPU contention
- Create multiple tasks with `kstep_task_create()` and distribute across CPUs 1-7
- Simulate DSQ contention by having multiple tasks repeatedly access shared scheduler state
- Use `on_tick_begin()` callback to monitor runqueue states and detect contention patterns
- Trigger simulated "bypass mode" by rapidly changing task affinities with `kstep_task_pin()`
- Monitor for live-locking via `kstep_tick_repeat()` timing and task migration delays
- Detect the bug by observing excessive delays in task movement and scheduling stalls
- Log contention events and timing anomalies to identify the live-locking condition
