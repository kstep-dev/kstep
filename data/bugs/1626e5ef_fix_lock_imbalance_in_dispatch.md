# sched_ext: Fix lock imbalance in dispatch_to_local_dsq()

- **Commit:** 1626e5ef0b00386a4fd083fa7c46c8edbd75f9b4
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** sched_ext (EXT scheduler)

## Bug Description

A lock imbalance warning is triggered in dispatch_to_local_dsq() when multiple tasks rapidly change CPU affinity. The kernel detects an attempt to release a lock when no more locks are held, manifesting as "WARNING: bad unlock balance detected!" The condition occurs when the function races with dispatch_dequeue() and loses the race, causing incorrect assumptions about which runqueue is currently locked.

## Root Cause

The dispatch_to_local_dsq() function performs complex rq lock switching to handle dispatching a task to a different runqueue. When a race with dispatch_dequeue() occurs, the function may transition through different locked rq states (rq → src_rq → dst_rq → rq), but it did not properly track which runqueue was actually locked at each step. After calling move_remote_task_to_local_dsq(), the code assumed dst_rq was locked, but when dispatch_dequeue() won the race, the task was already removed and dst_rq was never actually locked, leading to an extra raw_spin_rq_unlock() call on the wrong rq.

## Fix Summary

The fix introduces a locked_rq variable to explicitly track which runqueue is currently held by the lock throughout the function. Each time the code switches between rq locks, it updates locked_rq to reflect the new lock state. Critically, after calling move_remote_task_to_local_dsq(), it sets locked_rq = dst_rq to record that dst_rq is now locked, and uses locked_rq for the final unlock sequence to ensure the correct rq is unlocked regardless of whether the race condition occurred.

## Triggering Conditions

This bug requires sched_ext to be enabled and multiple tasks rapidly changing CPU affinity, creating race conditions in dispatch_to_local_dsq(). The function must attempt to dispatch a task to a different CPU's local DSQ (dst_rq ≠ src_rq), triggering the complex rq lock switching sequence (rq → src_rq → dst_rq → rq). Concurrently, dispatch_dequeue() must race and win, removing the task from the global DSQ before move_remote_task_to_local_dsq() can process it. This causes the function to incorrectly assume dst_rq is locked when it's actually not, leading to a double unlock on the wrong runqueue during the final cleanup.

## Reproduce Strategy (kSTEP)

Use 3+ CPUs with sched_ext enabled. Create multiple tasks with kstep_task_create() and rapidly change their CPU affinity using kstep_task_pin() to create contention on dispatch_to_local_dsq(). Set up tasks to frequently migrate between CPUs by pinning them to different CPU sets in rapid succession (e.g., pin task A to CPU 1, then immediately to CPU 2). Use kstep_tick_repeat() with short intervals to increase dispatch frequency and race probability. Monitor kernel logs via on_tick_begin() callback to detect "WARNING: bad unlock balance detected!" messages. The bug manifests as lockdep warnings when the function attempts to unlock the wrong rq, detectable through kernel warning messages in the output logs.
