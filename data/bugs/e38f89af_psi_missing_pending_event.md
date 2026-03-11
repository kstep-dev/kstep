# sched/psi: Fix possible missing or delayed pending event

- **Commit:** e38f89af6a13e895805febd3a329a13ab7e66fa4
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

When a pending event flag is set to indicate that an event should be generated, the old code would skip generating that event if the growth metric fell below the configured threshold. This caused pending events to be indefinitely delayed or lost. The bug prevents the PSI system from properly notifying userspace about sustained pressure conditions, even when events were previously deferred across window boundaries.

## Root Cause

The logic error occurs in the `update_triggers()` function. When checking if a new stall occurred, the code unconditionally skips the trigger if growth is less than threshold, without considering whether a pending event flag is already set from a previous window. The pending event flag was introduced to track deferred events that must eventually be signaled, but the threshold check bypassed this logic entirely.

## Fix Summary

The fix wraps the threshold check in a condition that first checks if `t->pending_event` is already set. If no pending event exists, the threshold check proceeds normally. If a pending event is already pending, the threshold check is skipped, allowing the event to be generated when the rate limit permits. This ensures that deferred events are always eventually signaled regardless of growth values.

## Triggering Conditions

The bug requires a PSI trigger with `pending_event` already set from a previous window, followed by a new stall event where growth falls below the configured threshold. Specifically:
1. A PSI trigger monitors CPU/memory/IO pressure with a specific threshold and window size
2. Initial stall activity sets `pending_event=true` but gets deferred due to rate limiting
3. In a subsequent window, new stall activity occurs but growth < threshold
4. The buggy code skips event generation despite the pending event flag
5. This creates a window where pressure events are lost or indefinitely delayed

The race occurs when rate limiting prevents immediate event delivery but subsequent low-growth periods prevent the pending event from being processed.

## Reproduce Strategy (kSTEP)

Configure at least 2 CPUs (driver uses CPU 0). In `setup()`:
- Create 2-3 tasks using `kstep_task_create()`
- Pin tasks to CPU 1 to create controlled pressure: `kstep_task_pin(task, 1, 1)`
- Configure PSI monitoring via `/proc/pressure/cpu` or cgroups with appropriate thresholds

In `run()`:
- Wake tasks to trigger initial pressure: `kstep_task_wakeup(task)`
- Use `kstep_tick_repeat(N)` to create sustained CPU pressure above threshold
- Briefly pause tasks: `kstep_task_pause(task)` then resume to create growth drop
- Monitor PSI state through `kstep_sysctl_write()` or custom logging

Use `on_tick_end()` callback to check PSI trigger state and detect when `pending_event` remains set but growth < threshold. Log PSI metrics to verify missing events.

Detection: Compare expected vs actual PSI event counts, checking for delayed/missing events when pressure conditions warrant notification.
