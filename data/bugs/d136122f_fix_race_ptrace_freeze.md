# sched: Fix race against ptrace_freeze_trace()

- **Commit:** d136122f58458479fd8926020ba2937de61d7f65
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition exists between `__schedule()` and `ptrace_freeze_traced()`. The ptrace function can change a task's state (from TASK_TRACED to __TASK_TRACED or vice versa) while holding only siglock, but the scheduler relies on the assumption that only current and ttwu() modify task->state. This causes the scheduler's check for whether to deactivate a task to become unreliable, potentially causing incorrect scheduling behavior or wrong accounting of task state.

## Root Cause

The original code performed a double-check of prev->state: it was read before acquiring the runqueue lock and again checked after acquiring the lock. If the two values matched, the code assumed the state was stable and safe to use. However, ptrace_freeze_traced() operates under a different lock (siglock) and can change task->state between these two reads, invalidating the double-check assumption. This breaks the ordering guarantees that the previous fix (dbfb089d360b) relied upon.

## Fix Summary

Instead of relying on a double-check and memory barriers to ensure prev->state is stable, the fix uses a control dependency: the load of prev->state is moved after acquiring the runqueue lock, and the condition is simplified to just check if prev_state is non-zero. This ensures that the load of prev->state happens-before the store to prev->on_rq becomes visible, providing the required ordering without assuming the state cannot change.

## Triggering Conditions

The race occurs in `__schedule()` when a task is being scheduled out while concurrently being ptraced. Specifically:
- A task must be in TASK_TRACED state (under ptrace control)
- The scheduler reads `prev->state` before acquiring rq->lock in `__schedule()`
- Between this read and the subsequent double-check after lock acquisition, `ptrace_freeze_traced()` changes the task state (TASK_TRACED ↔ __TASK_TRACED) while holding only siglock
- The original double-check logic (`prev_state == prev->state`) fails to detect this concurrent modification
- This breaks the assumption that only current task and ttwu() modify task->state, leading to incorrect deactivation decisions and potential accounting errors in the scheduler

## Reproduce Strategy (kSTEP)

Reproducing this race requires simulating the narrow timing window between ptrace operations and scheduler state checks:
- Setup: 2+ CPUs (CPU 0 reserved for driver), create a traced task using `kstep_task_create()`
- Use `kstep_task_pin()` to pin the traced task to a specific CPU, then force scheduling via `kstep_tick()`
- Monitor task state transitions using custom logging in `on_tick_begin()` callback to track `prev->state` reads
- Simulate ptrace state changes by directly modifying task->state between TASK_TRACED and __TASK_TRACED during the critical window
- Use `kstep_task_pause()` and `kstep_task_wakeup()` to trigger scheduler transitions while monitoring for inconsistent state handling
- Detection: Log prev->state before and after rq->lock acquisition; bug manifests as incorrect deactivation when states differ but double-check passes
- Verify fix by confirming the simplified control-dependency logic correctly handles concurrent ptrace modifications
