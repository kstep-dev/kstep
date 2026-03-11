# sched: Fix loadavg accounting race

- **Commit:** dbfb089d360b1cc623c51a2c7cf9b99eff78e0e7
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition exists in the task wakeup path where `p->state = TASK_WAKING` is set too early, before the scheduler (`__schedule()`) is guaranteed to stop reading `p->state`. This can cause the scheduler to read stale state values and incorrectly account task contributions to loadavg, breaking the accounting mechanism. The bug manifests as loadavg accounting errors and was reported by users experiencing real-world failures.

## Root Cause

The previous optimization commit c6e7bd7afaeb moved the `p->state = TASK_WAKING` assignment into the `p->on_rq == 0` check in `ttwu()`, assuming that once `schedule()` is entered, the current task's state cannot change. However, `schedule()` actually reads `p->state` multiple times during its execution, and the earlier `p->on_rq == 0` condition (compared to the old `p->on_cpu == 0` check) does not guarantee that `schedule()` has completed reading the state. This breaks the memory ordering guarantees required between the wakeup and scheduler paths.

## Fix Summary

The fix restores proper memory ordering by moving the `p->state = TASK_WAKING` assignment to after an acquire operation on `p->on_rq`, and by reading `p->state` early in `__schedule()` (before acquiring rq->lock) and comparing it after the lock is acquired to detect any concurrent modifications. It also moves the `sched_contributes_to_load` calculation from `ttwu()` back into `__schedule()`, ensuring correct loadavg accounting under all conditions.

## Triggering Conditions

The race occurs when `try_to_wake_up()` runs concurrently with `__schedule()` on different CPUs. Specifically:
- One CPU executes `__schedule()` and reads `prev->state` multiple times during scheduling
- Another CPU executes `ttwu()` and sets `p->state = TASK_WAKING` after checking `p->on_rq == 0`
- The `p->on_rq == 0` condition doesn't guarantee `__schedule()` has finished reading state
- Tasks must contribute to load (sleeping/blocked tasks) for loadavg accounting errors
- Race window exists between dequeue (`on_rq = 0`) and completion of `__schedule()`
- Most pronounced on SMP systems with weak memory ordering (ARM, PowerPC)

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (1-2 for tasks). Create tasks that repeatedly block and wake up:
- In `setup()`: Create 2 tasks with `kstep_task_create()`, pin to different CPUs with `kstep_task_pin()`
- In `run()`: Use `kstep_task_usleep()` to make tasks sleep (contribute to load), then `kstep_task_wakeup()`
- Create timing window: Have one task call `kstep_sleep()` while driver calls `kstep_task_wakeup()` on it
- Use `on_tick_begin()` callback to log task states and `sched_contributes_to_load` values
- Use `kstep_tick_repeat()` with short intervals to increase race probability
- Monitor for inconsistent loadavg accounting by checking if blocked tasks show wrong load contribution
- Detect bug by observing `prev->state` changes during `__schedule()` execution via custom logging
