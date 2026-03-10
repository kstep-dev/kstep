# sched/fair: Fixup wake_up_sync() vs DELAYED_DEQUEUE

- **Commit:** aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

When the DELAYED_DEQUEUE feature is enabled, sleeping tasks remain enqueued and counted in `rq->nr_running` until their lag has elapsed. In the `wake_affine_idle()` function, the code incorrectly uses `rq->nr_running` directly to determine if the current CPU only has one task, leading to wrong task placement decisions when delayed-dequeued tasks are present on the CPU.

## Root Cause

The `wake_affine_idle()` function checks if `rq->nr_running == 1` to decide whether to wake a task on the current CPU. However, `rq->nr_running` includes delayed-dequeued tasks (which are dequeued but still visible in the running count). This inflates the actual number of running tasks, causing the condition to fail when it should succeed, leading to suboptimal task affinity decisions.

## Fix Summary

The fix introduces a helper function `cfs_h_nr_delayed()` that calculates the actual number of delayed-dequeued tasks as `(rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable)`. In `wake_affine_idle()`, the check is updated from `rq->nr_running == 1` to `(rq->nr_running - cfs_h_nr_delayed(rq)) == 1`, ensuring the decision uses the actual count of truly running tasks.

## Triggering Conditions

- CFS scheduler with DELAYED_DEQUEUE feature enabled (kernel 6.12+)
- A sync wakeup scenario where `wake_affine_idle()` is called with sync=1
- Target CPU has exactly one truly running task plus one or more delayed-dequeued tasks
- Tasks with positive lag that get delayed-dequeued when they sleep
- Cache-sharing CPUs where affinity placement decision matters
- Waker task calls `wake_up_sync()` targeting a task on a CPU with delayed-dequeued tasks

## Reproduce Strategy (kSTEP)

- Setup: 2+ CPUs, create tasks A (waker) and B (wakee) on CPU 1, task C on CPU 2
- Use `kstep_task_pause(task_b)` to make task B accumulate positive lag, then sleep (delayed-dequeue)
- Verify delayed state: `rq->nr_running > rq->cfs.h_nr_runnable` on CPU 1
- Task A calls sync wakeup of task C: use `kstep_task_wakeup(task_c)` in sync context
- Check `wake_affine_idle()` decision via `on_tick_begin` callback logging
- Bug: Task C incorrectly not placed on CPU 1 due to inflated `nr_running` count
- Expected: With fix, `(nr_running - cfs_h_nr_delayed) == 1` should allow CPU 1 placement
