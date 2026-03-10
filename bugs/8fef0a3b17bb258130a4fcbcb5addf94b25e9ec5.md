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
