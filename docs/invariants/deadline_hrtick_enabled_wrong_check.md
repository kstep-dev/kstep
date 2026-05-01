# Hrtick Timer Armed Implies Scheduling-Class Feature Enabled
**Source bug:** `d16b7eb6f523eeac3cff13001ef2a59cd462aa73`

**Property:** If the hrtick timer is active on a runqueue, the corresponding scheduling-class-specific hrtick sched_feat (HRTICK_DL for deadline, HRTICK for fair) must be enabled.

**Variables:**
- `hrtick_active` — whether the rq's hrtick timer is armed. Read in-place via `hrtimer_active(&rq->hrtick_timer)` at the check point.
- `curr_class` — the scheduling class of the current task. Read in-place via `rq->curr->sched_class` at the check point.
- `hrtick_dl_feat` — whether `sched_feat(HRTICK_DL)` is enabled. Read in-place at the check point.
- `hrtick_fair_feat` — whether `sched_feat(HRTICK)` is enabled. Read in-place at the check point.

**Check(s):**

Check 1: Performed at the end of `set_next_task_dl()` (after `start_hrtick_dl` call site). Only when `first == true`.
```c
if (hrtimer_active(&rq->hrtick_timer) && rq->curr->sched_class == &dl_sched_class) {
    WARN_ON_ONCE(!sched_feat(HRTICK_DL));
}
```

Check 2: Performed at the end of `set_next_task_fair()` / `set_next_entity()` (after `hrtick_start_fair` call site).
```c
if (hrtimer_active(&rq->hrtick_timer) && rq->curr->sched_class == &fair_sched_class) {
    WARN_ON_ONCE(!sched_feat(HRTICK));
}
```

**Example violation:** The bug uses `hrtick_enabled(rq)` instead of `hrtick_enabled_dl(rq)` in `set_next_task_dl()`, so the hrtick timer gets armed for DL tasks even when `sched_feat(HRTICK_DL)` is false, violating the invariant.

**Other bugs caught:** None known, but this would catch any future regression where a refactoring accidentally drops the class-specific sched_feat guard from a `start_hrtick_*()` call site (e.g., a similar mistake in the CFS path).
