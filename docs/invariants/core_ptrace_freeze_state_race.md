# Sleeping task must be dequeued after voluntary schedule
**Source bug:** `d136122f58458479fd8926020ba2937de61d7f65`

**Property:** After `__schedule()` completes its deactivation decision for a non-preempted task, if the task's state is non-zero (sleeping) and was not reset to TASK_RUNNING by signal handling, the task must have been dequeued (`on_rq == 0`).

**Variables:**
- `prev_state` — the task's state as read in `__schedule()`. Recorded at `__schedule()` after `rq_lock`. Read from `prev->state` (volatile).
- `preempt` — whether this schedule call is a preemption. Passed as parameter to `__schedule()`.
- `prev->on_rq` — whether the task is still on the runqueue. Read in-place after the deactivation decision block.
- `prev->state` — the task's current state after the deactivation block. Read in-place to check if signal handling reset it to TASK_RUNNING.

**Check(s):**

Check 1: Performed at `__schedule()`, after the deactivation decision block (just before `pick_next_task`). Only when `!preempt`.
```c
// After the if (!preempt && prev_state) { ... } block completes:
if (!preempt && prev->state != TASK_RUNNING && prev->on_rq) {
    // VIOLATION: task is in a sleeping state but was not dequeued.
    // This means deactivate_task() was incorrectly skipped.
    WARN(1, "sched: task %s/%d in state %ld still on_rq after voluntary schedule\n",
         prev->comm, prev->pid, prev->state);
}
```

**Example violation:** `ptrace_freeze_traced()` changes a task's state from `TASK_TRACED` to `__TASK_TRACED` between two reads of `prev->state` in `__schedule()`. The double-check `prev_state == prev->state` fails, causing `deactivate_task()` to be skipped. The task remains on the runqueue in a sleeping state, leaking `calc_load_tasks` accounting.

**Other bugs caught:** `dbfb089d360b` (the predecessor "sched: Fix loadavg accounting race" introduced the vulnerable double-check pattern — any similar race where the state re-check falsely mismatches would be caught). Potentially also `core_loadavg_accounting_race` and `core_sched_switch_stale_state` if they involve missed deactivation.
