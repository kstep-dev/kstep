# Effective Uclamp Min Never Exceeds Effective Uclamp Max
**Source bug:** `0213b7083e81f4acd69db32cb72eb4e5f220329a`

**Property:** For any task on a runqueue, its effective `uclamp_min` must be less than or equal to its effective `uclamp_max`.

**Variables:**
- `eff_min` — the task's effective uclamp minimum value. Recorded at `uclamp_rq_inc_id` (called during enqueue). Read from `p->uclamp[UCLAMP_MIN].value` after `uclamp_eff_get()` computes it (this is the back-annotated active value stored on the task).
- `eff_max` — the task's effective uclamp maximum value. Recorded at `uclamp_rq_inc_id` (called during enqueue). Read from `p->uclamp[UCLAMP_MAX].value` after `uclamp_eff_get()` computes it.

Both values are already stored on the task struct as `p->uclamp[clamp_id].value` when the task is active (enqueued). No shadow variables needed — just read both fields and compare.

**Check(s):**

Check 1: Performed at `uclamp_rq_inc` (after both clamp IDs have been incremented during enqueue). Precondition: `sched_uclamp_used` is enabled and `p->sched_class->uclamp_enabled`.
```c
// After the for_each_clamp_id loop in uclamp_rq_inc completes:
unsigned int eff_min = p->uclamp[UCLAMP_MIN].value;
unsigned int eff_max = p->uclamp[UCLAMP_MAX].value;
if (eff_min > eff_max) {
    // INVARIANT VIOLATED: effective uclamp inversion
    WARN_ONCE(1, "uclamp inversion: task %s eff_min=%u > eff_max=%u",
              p->comm, eff_min, eff_max);
}
```

Check 2: Performed at `uclamp_update_active` (after re-computing effective values for active tasks when tg clamp changes). Same precondition.
```c
// After the for_each_clamp_id loop that does dec/inc:
unsigned int eff_min = p->uclamp[UCLAMP_MIN].value;
unsigned int eff_max = p->uclamp[UCLAMP_MAX].value;
if (eff_min > eff_max) {
    WARN_ONCE(1, "uclamp inversion after update: task %s eff_min=%u > eff_max=%u",
              p->comm, eff_min, eff_max);
}
```

**Example violation:** When a task with `uclamp_min=60%` belongs to a task group with `uclamp_max=50%`, the buggy code independently computes effective min=60% and effective max=50%, yielding `eff_min > eff_max`.

**Other bugs caught:** This invariant would also catch any future bug where uclamp min/max are computed or propagated independently without cross-checking, including stale values left by partial updates (the second part of this same commit's fix).
