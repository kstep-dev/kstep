# sched/fair: Fix overutilized update in enqueue_task_fair()

- **Commit:** 8e1ac4299a6e8726de42310d9c1379f188140c71
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The `enqueue_task_fair()` function attempts to skip the overutilized status update for new tasks since their `util_avg` is not yet accurate. However, the `flags` parameter used to check if a task is new gets modified earlier in the function, making the condition at the update check ineffective (a no-op). This causes new tasks to incorrectly trigger overutilized updates when they should be skipped, potentially disrupting task placement decisions made by Energy-Aware Scheduling (EAS).

## Root Cause

The `flags` parameter is checked late in the function with `if (flags & ENQUEUE_WAKEUP)` to determine if a task is new, but the `flags` variable has already been modified/overwritten earlier in the function. This causes the condition to not work as intended when it is evaluated, preventing the skip logic from functioning properly.

## Fix Summary

The fix saves the task newness state early in the function by introducing a local variable `int task_new = !(flags & ENQUEUE_WAKEUP)` before any modifications to `flags`. The overutilized update check then uses this saved variable (`if (!task_new)`) instead of checking the potentially-modified `flags` parameter directly.

## Triggering Conditions

The bug occurs during task enqueuing in `enqueue_task_fair()` when:
- A new task (without `ENQUEUE_WAKEUP` flag) is being scheduled
- The task goes through the scheduler hierarchy update logic which modifies the `flags` parameter
- The overutilized status update check at the end incorrectly treats the new task as an existing task due to the corrupted flags
- This causes new tasks to trigger overutilized updates when they should be skipped
- Most observable when Energy-Aware Scheduling (EAS) is active and task placement decisions depend on overutilized status

## Reproduce Strategy (kSTEP)

Create a scenario where new task creation incorrectly triggers overutilized updates:
- Setup: 2+ CPUs (CPU 0 reserved for driver), enable EAS-relevant conditions if possible
- Create initial load: Use `kstep_task_create()` and `kstep_task_wakeup()` to establish baseline CPU utilization
- Trigger bug: Create new tasks with `kstep_task_create()` (which internally calls enqueue without ENQUEUE_WAKEUP)
- Observe: Use `on_tick_begin()` callback to log overutilized status changes via custom logging
- Detection: Check if overutilized status updates occur when new tasks are created (should be skipped)
- Verification: Compare behavior between buggy and fixed kernels - buggy version shows spurious overutilized updates
