# sched_ext: fix flag check for deferred callbacks

- **Commit:** a3c4a0a42e61aad1056a3d33fd603c1ae66d4288
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext scheduler class)

## Bug Description

In the `schedule_deferred()` function, the wrong flag was being checked when determining whether a deferred balance callback was already pending. The code checked `SCX_RQ_BAL_PENDING` instead of `SCX_RQ_BAL_CB_PENDING`, which prevented proper detection of duplicate pending callback requests. This could lead to multiple redundant callback scheduling attempts, causing incorrect behavior in the deferred balance callback mechanism.

## Root Cause

The flag check was inconsistent with the corresponding flag that gets set later in the same function. When `SCX_RQ_IN_BALANCE` is true, the code sets `SCX_RQ_BAL_CB_PENDING` to signal that a balance callback should be deferred. However, the early guard check was looking for `SCX_RQ_BAL_PENDING` instead, which is a different flag used for a different purpose. This mismatch caused the guard condition to fail to prevent duplicate scheduling.

## Fix Summary

The fix changes the flag check in `schedule_deferred()` from `SCX_RQ_BAL_PENDING` to `SCX_RQ_BAL_CB_PENDING`, making it consistent with the flag that is actually set by the function. This ensures the function correctly detects when a deferred balance callback request is already pending and prevents redundant scheduling.

## Triggering Conditions

This bug occurs in the sched_ext scheduler class when `schedule_deferred()` is called multiple times during a balance operation. The specific conditions are:
- sched_ext scheduler must be active with balance operations occurring
- `SCX_RQ_IN_BALANCE` flag must be set (indicating an ongoing balance operation)  
- Multiple calls to `schedule_deferred()` on the same runqueue during the balance
- The incorrect flag check (`SCX_RQ_BAL_PENDING` instead of `SCX_RQ_BAL_CB_PENDING`) allows redundant callback scheduling
- This manifests as multiple `queue_balance_callback()` requests being scheduled when only one should be pending

## Reproduce Strategy (kSTEP)

Since this is a sched_ext specific bug, reproduction requires a sched_ext scheduler implementation. The strategy would be:
- Setup: Minimum 2 CPUs (CPU 0 reserved for driver, use CPU 1+ for tasks)
- Use `kstep_task_create()` to create multiple tasks to trigger load balancing
- Configure tasks with `kstep_task_pin()` to create load imbalance between CPUs
- Monitor sched_ext balance operations using `on_sched_balance_begin()` callback
- Use `kstep_tick_repeat()` to drive scheduler tick processing during balance operations  
- Check for redundant callback scheduling by logging when `SCX_RQ_BAL_CB_PENDING` is set multiple times
- Trigger the bug by ensuring concurrent calls to `schedule_deferred()` during balance
- Detection: Log flag state changes and count redundant `queue_balance_callback()` scheduling attempts
