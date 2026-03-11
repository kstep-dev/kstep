# sched: Fix TASK_state comparisons

- **Commit:** 5aec788aeb8eb74282b75ac1b317beb0fbb69a42
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The `state_filter_match()` function was using direct equality comparison (`state == TASK_IDLE`) to identify idle tasks when filtering for `TASK_UNINTERRUPTIBLE` state. However, task state is fundamentally a bitmask that can have multiple modifiers combined (such as `TASK_NOLOAD`, `TASK_KILLABLE`, `TASK_FREEZABLE`). The direct comparison fails to account for these modifiers, causing incorrect task state matching and breaking the intended filtering logic.

## Root Cause

Task states can be combined with modifiers using bitwise operations, but the original code was using a simple equality check that only matches exact state values. When modifiers like `TASK_NOLOAD` are combined with base states, the resulting bitmask no longer equals the base state value, causing the comparison to fail. The introduction of `TASK_FREEZABLE` as a state modifier exacerbated this issue, breaking the `__wait_is_interruptible()` logic which depends on correct state matching.

## Fix Summary

The fix changes the equality comparison to a bitwise AND check: `(state & TASK_NOLOAD)` instead of `state == TASK_IDLE`. This correctly identifies when the `TASK_NOLOAD` modifier is set regardless of other bits in the task state bitmask, ensuring proper task filtering in the presence of state modifiers.

## Triggering Conditions

The bug requires tasks with combined state modifiers that break equality comparisons:
- Tasks in `TASK_UNINTERRUPTIBLE | TASK_FREEZABLE` state (freezable uninterruptible sleep)
- Tasks in `TASK_INTERRUPTIBLE | TASK_FREEZABLE` state (freezable interruptible sleep)  
- Tasks in `TASK_IDLE` with `TASK_NOLOAD` modifier when filtered by `state_filter_match()`
- Code paths that call `___wait_is_interruptible()` expecting proper bitmask checking
- Hung task detector scanning processes with `TASK_UNINTERRUPTIBLE | TASK_KILLABLE` combinations
- Race condition where `TASK_FREEZABLE` modifier causes state comparison failures

## Reproduce Strategy (kSTEP)

Set up freezable tasks and invoke state filtering logic that uses broken equality comparisons:
- **CPUs needed**: 2 (driver on CPU 0, tasks on CPU 1)
- **Setup**: Create multiple tasks with `kstep_task_create()`, put them in different wait states
- **Sequence**: Use `kstep_freeze_task()` to create freezable states, then call kernel functions that trigger `state_filter_match()` and `___wait_is_interruptible()` 
- **Callbacks**: Use `on_tick_begin()` to monitor task states via `READ_ONCE(task->__state)`
- **Detection**: Log when equality checks `(state == TASK_INTERRUPTIBLE)` fail but bitwise checks `(state & TASK_INTERRUPTIBLE)` succeed; compare hung task detector behavior before/after fix
- **Validation**: Verify state filtering works correctly with combined modifiers like `TASK_UNINTERRUPTIBLE | TASK_FREEZABLE`
