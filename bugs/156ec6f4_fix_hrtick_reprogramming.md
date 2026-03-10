# sched/features: Fix hrtick reprogramming

- **Commit:** 156ec6f42b8d300dbbf382738ff35c8bad8f4c3a
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

Hung tasks and RCU stall cases occurred on non-100%-busy systems due to corruption of the rbtree used by the hrtimer base for hrtick. The periodic sched tick timer was lost from the corrupted tree, causing all machinery dependent on it (timers, RCU, etc.) to stop working. This issue manifested only when HRTICK was enabled.

## Root Cause

A race condition existed between `hrtimer_set_expires()` in `hrtick_start()` and `hrtimer_start_expires()` in `__hrtick_restart()`. The former could operate on an hrtick hrtimer that was already queued by the latter, leading to modification of the timer while it was already in the rbtree, which corrupted the tree structure. The operations were not fully serialized by the base lock.

## Fix Summary

The fix stores the calculated hrtick expiration time in a new field `rq->hrtick_time` and uses `hrtimer_start()` instead of `hrtimer_set_expires()` followed by `hrtimer_start_expires()`. This ensures the entire hrtimer reprogramming operation is atomically performed under the base lock, eliminating the race window.

## Triggering Conditions

- **HRTICK feature must be enabled** (CONFIG_SCHED_HRTICK=y)
- **SMP system** where hrtick timers can be reprogrammed from different contexts
- **Race between** `hrtick_start()` and `__hrtick_restart()` on the same rq's hrtick_timer
- **Timer already queued** in hrtimer rbtree when `hrtimer_set_expires()` attempts modification
- **Non-100% busy system** where hrtick operations occur frequently enough to trigger the race
- **Timing window** where `hrtimer_set_expires()` operates on a timer that `hrtimer_start_expires()` has already queued but not yet removed from the rbtree
- **Base lock insufficient** to serialize the split operation of set_expires + start_expires

## Reproduce Strategy (kSTEP)

- **CPUs needed**: At least 2 CPUs (CPU 0 reserved for driver)
- **Setup**: Enable HRTICK via sysctl, create multiple CFS tasks with different priorities to trigger frequent hrtick reprogramming
- **Sequence**: Use `kstep_task_create()` for 4-6 tasks, `kstep_task_set_prio()` for varying priorities, pin tasks to CPU 1-2 with `kstep_task_pin()`
- **Trigger race**: Rapidly alternate between `kstep_task_wakeup()` and `kstep_task_pause()` to force frequent hrtick rescheduling
- **Timing**: Use `kstep_tick()` with short intervals and `kstep_task_usleep()` to create timing windows for the race
- **Detection**: Monitor hrtimer base corruption via `on_tick_begin()` callback checking for missing periodic tick, or kernel hangs/stalls
- **Observation**: Log hrtimer state changes and detect rbtree inconsistencies when periodic timer gets "lost"
- **Failure mode**: System hangs, RCU stalls, or timer subsystem stops working entirely
