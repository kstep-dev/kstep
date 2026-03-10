# sched,psi: Handle potential task count underflow bugs more gracefully

- **Commit:** 9d10a13d1e4c349b76f1c675a874a7f981d6d3b4
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

The `psi_group_cpu->tasks` counter, stored as an unsigned int, is decremented without checking if it is already zero. When a decrement occurs at zero, unsigned integer wrapping causes the counter to become a large positive value, incorrectly triggering pressure state flags in `psi_group_cpu->state_mask`. This leads to spurious PSI events that cause unnecessary time sampling and wrong actions being taken at user land based on these false pressure notifications.

## Root Cause

The original code unconditionally decrements `groupc->tasks[t]--` even when the counter is zero. Since `tasks[t]` is an unsigned integer, decrementing zero wraps around to a very large value (e.g., UINT_MAX). This causes the subsequent state mask calculation to incorrectly mark pressure states as active, even though no tasks are actually stalled on that resource.

## Fix Summary

The fix guards the decrement operation with a check for non-zero values: only decrement when `groupc->tasks[t]` is positive. If the counter is zero, the error is logged (if psi_bug is not already set) but no underflow occurs. This prevents the integer wrapping and ensures spurious PSI events are not generated.

## Triggering Conditions

The bug occurs in `psi_group_change()` when the PSI subsystem attempts to decrement task counters for pressure state transitions (IO/memory/CPU waiting). Key conditions:
- A task state transition that should decrement `psi_group_cpu->tasks[t]` for some pressure type `t` 
- The corresponding counter `groupc->tasks[t]` is already zero when the decrement occurs
- This creates an unsigned integer underflow (0 - 1 = UINT_MAX), causing `state_mask` calculation to incorrectly show pressure
- Race condition: Multiple rapid task state changes or incorrect tracking can lead to unbalanced increment/decrement operations
- Triggers spurious PSI events and time sampling that doesn't reflect actual resource pressure

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved). Create tasks that rapidly transition between pressure states to cause unbalanced PSI counter updates:
- In `setup()`: Use `kstep_task_create()` to create 2-3 tasks on different CPUs via `kstep_task_pin()`
- In `run()`: Force rapid sleep/wakeup cycles with `kstep_task_pause()` and `kstep_task_wakeup()` to trigger PSI state changes
- Use `kstep_tick_repeat()` with small intervals to accelerate scheduling and PSI updates  
- Monitor via `on_tick_end()` callback to check PSI group CPU state and task counters using direct kernel structure access
- Detect bug by checking for underflow: large `psi_group_cpu->tasks[t]` values (near UINT_MAX) when no tasks should be in pressure states
- Log spurious pressure state mask bits that shouldn't be set given actual task states
