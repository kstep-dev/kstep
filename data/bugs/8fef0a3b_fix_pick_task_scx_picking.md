# sched_ext: Fix pick_task_scx() picking non-queued tasks when it's called without balance()

- **Commit:** 8fef0a3b17bb258130a4fcbcb5addf94b25e9ec5
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

A previous workaround in pick_task_scx() incorrectly tested whether the previous task was on the SCX scheduler class to decide whether to keep it running. However, a task can be on SCX but no longer runnable (not queued). This causes pick_task_scx() to return non-runnable tasks, leading to a (!on_rq && on_cpu) state where the task is on the CPU but not on any runqueue. This state can cause potential wakers to busy loop, easily leading to deadlocks and other scheduling confusions.

## Root Cause

The workaround code checked `prev->sched_class == &ext_sched_class` to determine if a task should be kept running. This check only verifies the task's scheduler class, not whether it's actually queued and runnable. A task may belong to the SCX scheduler class but have been dequeued and made non-runnable, violating the invariant that a task on the CPU must be on some runqueue.

## Fix Summary

Replace the scheduler class check with a check for the SCX_TASK_QUEUED flag (`prev->scx.flags & SCX_TASK_QUEUED`), which correctly identifies whether the task is actually queued in the SCX scheduler. This ensures only runnable tasks are returned from pick_task_scx(), preventing the invalid (!on_rq && on_cpu) state and potential deadlocks.

## Triggering Conditions

The bug triggers when pick_task_scx() is called without a preceding balance_scx() call (due to a fair scheduler bug where pick_task_fair() returns NULL after balance_fair() returns true). This sets SCX_RQ_BAL_PENDING flag on the runqueue. The bug occurs when the previous task belongs to the SCX scheduler class but has been dequeued (SCX_TASK_QUEUED flag unset), causing the workaround code to incorrectly return a non-runnable task. This leads to the invalid (!on_rq && on_cpu) state where the task is on CPU but not on any runqueue. The timing condition requires the task to transition from runnable to non-runnable while still assigned to the SCX scheduler class, which can happen during task state changes or when tasks are moved between scheduler classes.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). In setup(), create a custom sched_ext scheduler program that can dequeue tasks while keeping them in SCX class. Create two SCX tasks on CPU 1. In run(), trigger the race condition by having one SCX task become non-runnable (e.g., sleep or block) while still belonging to ext_sched_class, then force pick_task_scx() to be called without balance_scx() by manipulating the scheduler state to set SCX_RQ_BAL_PENDING. Use on_tick_begin() callback to monitor task states and check for the (!on_rq && on_cpu) condition. Log task->state, task->on_rq, task->on_cpu, and task->scx.flags to detect when pick_task_scx() returns a non-queued task. The bug manifests when a task with SCX_TASK_QUEUED unset is returned, leading to potential waker busy-loops and scheduling deadlocks.
