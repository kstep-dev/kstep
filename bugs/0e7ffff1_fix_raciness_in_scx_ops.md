# scx: Fix raciness in scx_ops_bypass()

- **Commit:** 0e7ffff1b8117b05635c87d3c9099f6aa9c9b689
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

A race condition occurs in `scx_ops_bypass()` where concurrent calls on the ops enable/disable path can corrupt the `scx_ops_bypass_depth` counter and cause inconsistent scheduler state. When one CPU checks `scx_ops_bypass_depth` and decides to iterate over all CPUs to update their scheduler state, another CPU can modify the depth counter in between, causing the iteration to execute even though a previous call already triggered it. This results in kernel warnings (BUG_ON violations) in the scheduler code during sched_ext selftest runs.

## Root Cause

The original code used atomic operations (`atomic_inc_return()` and `atomic_dec_return()`) on `scx_ops_bypass_depth`, which made the individual depth modification atomic. However, the decision to iterate over all CPUs and update their state is made after checking the depth value. Between this check and the subsequent CPU iteration, another thread can modify the depth counter, causing two concurrent iterations with inconsistent state. The race occurs because atomic operations alone cannot protect the check-then-act pattern spanning the CPU iteration loop.

## Fix Summary

The fix replaces the atomic `scx_ops_bypass_depth` with a regular integer protected by a raw spinlock (`__scx_ops_bypass_lock`). This raw spinlock cannot be preempted and safely synchronizes the depth check with the entire CPU iteration, ensuring that only the code path that triggers a state change (depth transitioning to/from 1 or 0) performs the iteration. Additionally, the rq locking is changed from `rq_lock_irqsave()` to `rq_lock()` since interrupts are already disabled by the raw spinlock.

## Triggering Conditions

The race requires concurrent execution on multiple CPUs during sched_ext scheduler enable/disable operations. Specifically:
- CPU A calls `scx_ops_bypass(true)` on the enable path, incrementing bypass depth to 1
- An operation on the init path exits, scheduling `scx_ops_disable_workfn()` workqueue
- CPU B calls `scx_ops_bypass(false)` on disable path, decrementing depth to 0  
- The disable workfn kthread gets scheduled and calls `scx_ops_bypass(true)` again
- CPUs A and B race during the CPU iteration loop in `scx_ops_bypass()`
- The atomic increment/decrement of `scx_ops_bypass_depth` completes successfully, but the subsequent check-then-act pattern (checking depth value, then iterating over CPUs) allows concurrent modifications
- Race window exists between reading the depth counter and completing the per-CPU scheduler state updates
- Triggers BUG_ON assertions in scheduler code when multiple threads attempt concurrent CPU iterations

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved for driver, CPUs 1-2 for race reproduction). In `setup()`, initialize minimal topology with `kstep_topo_init()` and `kstep_topo_apply()`. Create multiple kernel threads using `kstep_kthread_create()` to simulate concurrent sched_ext operations. In `run()`, use `kstep_kthread_bind()` to pin threads to different CPUs, then trigger rapid sched_ext enable/disable cycles by alternating calls that would invoke `scx_ops_bypass(true)` and `scx_ops_bypass(false)`. Since sched_ext is not directly exposed via kSTEP, the driver may need to directly manipulate internal scheduler state or trigger operations that lead to bypass calls. Use `on_tick_begin()` and `on_sched_softirq_end()` callbacks to monitor scheduler state during the race window. Detection involves checking for BUG_ON warnings in kernel logs or observing inconsistent `scx_ops_bypass_depth` values across CPUs. The race is timing-sensitive, so use `kstep_tick_repeat()` with short intervals and potentially repeat the sequence multiple times to increase race probability.
