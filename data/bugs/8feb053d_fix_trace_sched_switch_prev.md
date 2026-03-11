# sched: Fix trace_sched_switch(.prev_state)

- **Commit:** 8feb053d53194382fcfb68231296fdc220497ea6
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When a task has signals pending during context switching, the `try_to_block_task()` function updates the task's internal state to TASK_RUNNING, but this state change was not propagated back to the caller's local variable. As a result, the tracepoint for sched_switch events would see a stale/incorrect task state instead of the updated TASK_RUNNING state, causing system monitoring and tracing tools to log incorrect task state information.

## Root Cause

The `try_to_block_task()` function previously took `task_state` as a value parameter rather than a pointer. When `signal_pending_state()` returned true, the function would update the task's `__state` field to TASK_RUNNING via `WRITE_ONCE()`, but the caller's `prev_state` variable remained unchanged. Since the tracepoint uses this caller-side variable, it would observe the stale state.

## Fix Summary

The fix changes `try_to_block_task()` to accept a pointer to `task_state` instead of a value, allowing the function to update the caller's state variable when it changes the task's state to TASK_RUNNING. This ensures the tracepoint sees the correct, up-to-date task state.

## Triggering Conditions

The bug occurs during context switching in `__schedule()` when:
- A task attempts to block (enter sleep state like TASK_INTERRUPTIBLE)
- The task has pending signals that make `signal_pending_state()` return true
- `try_to_block_task()` gets called and updates the task's internal `__state` to TASK_RUNNING
- However, the caller's `prev_state` variable remains unchanged at the original blocking state
- The `trace_sched_switch` tracepoint uses the stale `prev_state` instead of TASK_RUNNING
- This creates a discrepancy between the actual task state and what monitoring tools observe

The race requires precise timing: the signal must arrive after the task decides to block but before the context switch completes.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create a task that will attempt to sleep, then send it a signal:

1. In `setup()`: Create a task with `kstep_task_create()` and pin it to CPU 1
2. In `run()`: Make the task sleep with `kstep_task_usleep()` for a long duration (e.g., 10000us)
3. Immediately after, use signal injection (if available in kSTEP) or simulate via direct kernel manipulation
4. Use `on_tick_begin()` callback to monitor task state transitions and log both `task->__state` and the state passed to tracepoints
5. Detection: Compare the task's actual `__state` field vs. the state seen by trace events
6. Bug manifests when trace shows sleeping state but task is actually TASK_RUNNING due to signal handling

The key is catching the moment when `signal_pending_state()` triggers during the sleep attempt, revealing the state propagation bug.
