# rcu/tasks: Fix stale task snapshot for Tasks Trace

- **Commit:** 399ced9594dfab51b782798efe60a2376cd5b724
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition in the RCU-tasks-trace pre-grace-period snapshot mechanism allows newly running tasks to miss pre-GP update-side accesses from other CPUs. When one CPU performs an update followed by a context switch on another CPU, the new task on the second CPU may not observe the update due to insufficient synchronization ordering. This can violate RCU-tasks-trace's guarantee that a task snapshot captures coherent state relative to update-side accesses.

## Root Cause

The `cpu_curr_snapshot()` function performed only simple memory barriers (`smp_mb()`) without locking the runqueue, leaving a race window where a context switch could occur without proper ordering with pre-GP updates. Specifically, the new task could start executing and perform RCU reads before the snapshot function completed, creating a situation where neither the snapshot saw the new task nor the new task saw the pre-GP update.

## Fix Summary

The fix adds runqueue locking (`rq_lock_irqsave()` and `rq_unlock_irqrestore()`) around the task snapshot in `cpu_curr_snapshot()`. By holding the runqueue lock during the snapshot, the update side is guaranteed to see either the old task (if context switch hasn't occurred) or the new task (if it has), and any subsequent context switches will see the pre-GP update accesses. Additionally, `smp_mb__after_spinlock()` replaces the initial `smp_mb()` to leverage the spinlock's own implicit barriers.

## Triggering Conditions

This bug requires precise timing between an RCU-tasks-trace grace period start on one CPU and a context switch on another CPU. The race occurs when:
- CPU 0 performs a pre-GP memory update followed by `smp_mb()` and calls `cpu_curr_snapshot()` on CPU 1
- CPU 1 simultaneously performs a context switch, updating `rq->curr` and unlocking the runqueue 
- The new task on CPU 1 starts executing and performs RCU read-side operations before CPU 0's snapshot completes
- Without runqueue locking, neither the snapshot observes the new task nor does the new task observe the pre-GP update
- This violates RCU-tasks-trace ordering guarantees that ensure either visibility of the task or the update

## Reproduce Strategy (kSTEP)

Use 2+ CPUs and create a tight race between RCU snapshot and context switch:
- **Setup**: Create two competing tasks pinned to CPU 1, use `kstep_task_pause()` and `kstep_task_wakeup()` to control scheduling
- **Race simulation**: Pin one task to CPU 1, then pause it mid-execution to trigger a context switch
- **Timing control**: Use `kstep_tick()` and `kstep_sleep()` to synchronize the snapshot timing with context switches
- **Detection**: Monitor via `on_tick_begin()` callbacks to log task switches and check for missed updates 
- **Verification**: Log when snapshot occurs vs when new task starts executing to detect ordering violations
- **Observable behavior**: Look for cases where a newly scheduled task misses seeing prior memory updates that should be visible
