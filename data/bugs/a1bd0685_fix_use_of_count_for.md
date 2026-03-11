# sched: Fix use of count for nr_running tracepoint

- **Commit:** a1bd06853ee478d37fae9435c5521e301de94c67
- **Affected file(s):** kernel/sched/sched.h
- **Subsystem:** Core scheduler tracing

## Bug Description

The `sub_nr_running()` function is supposed to decrement the run queue's nr_running counter and emit a tracepoint event with a count value indicating whether it's an add (positive) or subtract (negative) operation. However, the tracepoint was passing the raw positive count value instead of the negated count, causing the tracepoint to report subtract operations as positive additions. This results in incorrect trace data for monitoring and debugging tools that rely on the sign of the count field to determine the operation type.

## Root Cause

In the `sub_nr_running()` function, the call to `call_trace_sched_update_nr_running(rq, count)` was missing the negation of the count parameter. The count field is semantically meant to indicate the direction and magnitude of change to nr_running—negative for decrements and positive for increments—but the code was unconditionally passing the unsigned count value without negation, violating this contract.

## Fix Summary

The fix adds a minus sign to the count parameter in the tracepoint call: `call_trace_sched_update_nr_running(rq, -count)`. This ensures that when nr_running is decremented, the tracepoint correctly reports a negative change value, allowing trace consumers to accurately distinguish between add and subtract operations.

## Triggering Conditions

This bug manifests whenever tasks are removed from runqueues, triggering the `sub_nr_running()` function. The specific conditions include:
- Any task state transition that decreases nr_running (task exit, sleep, block, pause, migration)
- Both CFS and RT/DL tasks being dequeued from any CPU's runqueue
- No specific CPU topology, timing, or race conditions required
- Bug is deterministic and occurs on every task removal operation
- The incorrect tracing data affects any monitoring tools that parse sched_update_nr_running tracepoint events to track runqueue dynamics

## Reproduce Strategy (kSTEP)

This bug requires tracepoint validation rather than scheduler behavior changes. A reproduction strategy:
- **CPUs needed**: Minimum 2 (CPU 0 reserved for driver, use CPU 1 for test tasks)
- **Setup**: Create 2-3 test tasks using `kstep_task_create()`
- **Run sequence**: Use `kstep_task_wakeup()` to wake tasks, then `kstep_task_pause()` to trigger `sub_nr_running()`
- **Detection method**: Implement custom tracing callback or patch `sub_nr_running()` to log the count parameter value passed to the tracepoint
- **Verification**: Check that count values are positive when they should be negative (indicating the bug), or properly negative after the fix
- **Key observation**: In buggy kernel, task removals incorrectly report positive count values; in fixed kernel, they correctly report negative values
