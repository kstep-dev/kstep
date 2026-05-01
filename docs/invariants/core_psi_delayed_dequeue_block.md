# PSI Flags Consistent With Task CPU Execution State
**Source bug:** `f5aaff7bfa11fb0b2ee6b8fd7bbc16cfceea2ad3`

**Property:** After a context switch where the previous task blocked (entered sleep), `TSK_RUNNING` must not remain set in the previous task's `psi_flags`.

**Variables:**
- `prev_blocked` — whether the previous task entered the blocking path in `__schedule()`. Recorded at `__schedule()`, immediately after `block_task()` is called. This is a boolean derived from control flow (did we reach the `block_task()` call?).
- `prev->psi_flags` — the PSI state flags of the previous task. Read directly from `task_struct->psi_flags` after the context switch completes.
- `TSK_RUNNING` — the PSI flag indicating a task is running (value `1 << NR_RUNNING`, i.e., 4). Defined in `linux/psi_types.h`.

**Check(s):**

Check 1: Performed at `psi_sched_switch()` (called from `__schedule()` just before `context_switch()`). Precondition: a context switch is occurring (`prev != next`).
```c
// If the previous task entered the blocking path, PSI must be told
// it is sleeping so that TSK_RUNNING gets cleared.
// The 'sleep' argument to psi_sched_switch() must be true when the
// task blocked, regardless of whether it remains physically queued
// (e.g., due to delayed dequeue).
if (prev_blocked) {
    WARN_ON_ONCE(sleep == false);
}
```

Check 2: Performed after `finish_task_switch()` returns (i.e., when the new task resumes and finishes the switch on behalf of `prev`). Precondition: `prev` blocked during the context switch.
```c
// After a blocking context switch completes, the previous task
// must not have TSK_RUNNING set in its psi_flags.
if (prev_blocked && (prev->psi_flags & TSK_RUNNING)) {
    WARN_ON_ONCE(1); // PSI flags inconsistent: TSK_RUNNING set on sleeping task
}
```

**Example violation:** With delayed dequeue, a blocking task remains on the runqueue (`task_on_rq_queued()` returns true). The old code passed `!task_on_rq_queued(prev)` as the `sleep` argument to `psi_sched_switch()`, yielding `false` even though the task logically blocked. This left `TSK_RUNNING` set in `psi_flags`, violating the invariant. On subsequent wakeup, PSI detected the double-set and emitted "psi: inconsistent task state!".

**Other bugs caught:** Potentially any future bug where a new dequeue-deferral mechanism (analogous to delayed dequeue) causes `task_on_rq_queued()` to diverge from the task's logical sleep state, breaking PSI accounting.
