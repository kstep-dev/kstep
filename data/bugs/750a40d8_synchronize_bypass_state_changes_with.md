# sched_ext: Synchronize bypass state changes with rq lock

- **Commit:** 750a40d816de1567bd08f1017362f6e54e6549dc
- **Affected file(s):** kernel/sched/ext.c, kernel/sched/sched.h
- **Subsystem:** EXT (sched_ext)

## Bug Description

While the BPF scheduler is being unloaded, NOHZ tick-stop error messages trigger: "NOHZ tick-stop error: local softirq work is pending, handler #80!!!". This occurs because the CPU enters idle with pending softirqs. The root cause is that the bypass state transition (entering bypass mode where the BPF scheduler is ignored) is not synchronized with rq operations, allowing the enqueue and dispatch paths to have different views of whether bypass mode is enabled.

## Root Cause

The bypass state was tracked globally via `scx_ops_bypassing()` which reads an atomic counter without synchronization against the rq lock. This creates a race condition: the enqueue path may check `scx_ops_bypassing()` and decide to use the BPF scheduler, while simultaneously the bypass transition updates the global state without holding the rq lock, causing the dispatch path to bypass the BPF scheduler. This synchronization gap leaves pending softirqs that prevent the tick from stopping.

## Fix Summary

The fix changes bypass state tracking from a global atomic counter to a per-rq flag (`SCX_RQ_BYPASSING`) that is modified while the rq lock is held. This ensures that enqueue and dispatch paths have a consistent view of bypass mode within the same rq critical section, eliminating the race condition.

## Triggering Conditions

The bug occurs during BPF scheduler unloading when the bypass mode transition is not synchronized with rq operations. Specifically:
- The sched_ext scheduler is active and BPF scheduler unloading is triggered
- The global atomic `scx_ops_bypass_depth` counter transitions from 0→1 without holding rq locks
- Race condition: enqueue path reads old bypass state (0) and attempts BPF scheduling
- Simultaneously, dispatch path reads new bypass state (1) and bypasses BPF scheduler
- This leaves pending softirqs from incomplete BPF operations, preventing NOHZ tick-stop
- CPU enters idle with pending softirqs, triggering "NOHZ tick-stop error" warnings

## Reproduce Strategy (kSTEP)

Reproducing this requires simulating the BPF scheduler unloading race condition:
- Setup: 2+ CPUs needed (driver uses CPU 0), enable sched_ext with custom BPF scheduler
- Create tasks in `setup()` using `kstep_task_create()` and assign to sched_ext class
- In `run()`: Start task scheduling with `kstep_task_wakeup()` and `kstep_tick_repeat()`
- Trigger bypass transition by directly manipulating `scx_ops_bypass_depth` without rq locks
- Use `on_tick_begin()` callback to log when enqueue/dispatch paths check bypass state
- Monitor softirq state via kernel symbols and detect pending work during idle transitions
- Check for NOHZ tick-stop errors in kernel logs to confirm bug reproduction
- Compare behavior before/after applying the `SCX_RQ_BYPASSING` per-rq flag fix
