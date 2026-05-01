# Trace prev_state Consistency With on_rq Status
**Source bug:** `8feb053d53194382fcfb68231296fdc220497ea6`

**Property:** At `trace_sched_switch`, if the switch is not a preemption and the previous task remains on the runqueue (`prev->on_rq`), then the reported `prev_state` must be `TASK_RUNNING` (0).

**Variables:**
- `prev_state` — the task state argument passed to `trace_sched_switch`. Recorded at `trace_sched_switch` tracepoint probe. Read directly from the tracepoint callback's `prev_state` parameter.
- `prev_on_rq` — whether `prev` is still enqueued. Recorded at `trace_sched_switch` tracepoint probe. Read from `prev->on_rq` inside the callback.
- `preempt` — whether this context switch is a preemption. Recorded at `trace_sched_switch` tracepoint probe. Read directly from the tracepoint callback's `preempt` parameter.

**Check(s):**

Check 1: Performed at `trace_sched_switch` tracepoint callback. Precondition: `!preempt && prev->on_rq`.
```c
// Inside a trace_sched_switch probe:
// If the task was not preempted and was not dequeued (still on_rq),
// then it must have been set back to TASK_RUNNING (e.g., by signal_pending_state).
// The reported prev_state must reflect this.
if (!preempt && prev->on_rq && prev_state != TASK_RUNNING) {
    // VIOLATION: prev_state is stale — task stayed on rq but
    // tracepoint reports a sleep state.
    pr_err("invariant violated: prev_state=%lu but prev->on_rq=%d\n",
           prev_state, prev->on_rq);
}
```

**Example violation:** A task sets `TASK_INTERRUPTIBLE` and calls `schedule()`, but `signal_pending_state()` in `try_to_block_task()` resets `__state` to `TASK_RUNNING` without updating the local `prev_state` variable. The tracepoint fires with `prev_state=TASK_INTERRUPTIBLE` while the task remains on the runqueue (`on_rq=1`), violating the invariant.

**Other bugs caught:** None known, but this would catch any future code path in `__schedule()` that keeps a task on the runqueue while failing to synchronize the `prev_state` variable passed to the tracepoint.
