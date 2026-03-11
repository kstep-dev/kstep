# sched/deadline: Fix task_struct reference leak

- **Commit:** b58652db66c910c2245f5bee7deca41c12d707b9
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

The deadline scheduler has a reference count leak in the task_struct when a deadline timer is canceled before it expires. When `enqueue_task_dl()` is called and finds a throttled task that was previously running a deadline timer, it cancels the timer using `hrtimer_try_to_cancel()`. However, when the cancel succeeds (timer was still pending), the corresponding `put_task_struct()` call that should happen in the timer callback is never executed, causing the reference count to leak and the task_struct to never be freed. This manifests as kmemleak reporting unreferenced task_struct objects during stress testing.

## Root Cause

In `start_dl_timer()`, a reference count is incremented via `get_task_struct(p)` and a timer is set. The timer callback `dl_task_timer` is supposed to decrement this reference in a normal flow. However, when `enqueue_task_dl()` calls `hrtimer_try_to_cancel()` on a throttled task's timer before it fires, the cancellation succeeds but there is no corresponding `put_task_struct()` call to balance the earlier `get_task_struct()`. This breaks the reference counting contract, leaving the task_struct with an elevated reference count that prevents it from being freed.

## Fix Summary

The fix adds a `put_task_struct()` call in `enqueue_task_dl()` when the timer is successfully canceled (when `hrtimer_try_to_cancel()` returns 1), but only for non-server deadline tasks. This ensures the reference count is properly decremented when the timer is canceled before expiration, preventing the leak.

## Triggering Conditions

A deadline task must be throttled (runtime exhausted) and have an active replenish timer via `start_dl_timer()`. The task must then be boosted via priority inheritance (PI) while throttled, triggering `enqueue_task_dl()` with `is_dl_boosted()` true. The timer cancellation in `enqueue_task_dl()` must succeed (timer still pending), meaning `hrtimer_try_to_cancel()` returns 1. This race occurs when the PI boost happens before the replenish timer expires, leaving the task_struct reference count elevated without the corresponding decrement from `dl_task_timer()`.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. In `setup()`: Create a deadline task with tight runtime/period parameters using SCHED_DEADLINE. In `run()`: Start the deadline task to exhaust its runtime and trigger throttling via `start_dl_timer()`. Immediately pause the task with `kstep_task_pause()` to simulate PI boost conditions, then call `kstep_task_wakeup()` to trigger `enqueue_task_dl()` with the timer still pending. Use `on_tick_begin()` callback to log task reference counts and timer states. Monitor for reference count mismatches - the leak manifests as task_struct objects with elevated reference counts that persist after task termination. Check `/proc/slabinfo` for task_struct slab growth to detect the leak.
