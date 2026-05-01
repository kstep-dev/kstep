# Idle Cgroup Preemption Consistency
**Source bug:** `faa42d29419def58d3c3e5b14ad4037f0af3b496`

**Property:** After wakeup preemption check (`check_preempt_wakeup_fair`), if the matched scheduling entity of the current task is idle and the matched entity of the waking task is non-idle, then `TIF_NEED_RESCHED` must be set on the current task.

**Variables:**
- `cse_is_idle` — Whether the current task's matched scheduling entity (after `find_matching_se`) is idle. Recorded at `check_preempt_wakeup_fair`, after `find_matching_se(&se, &pse)`. Obtained via `se_is_idle(se)` on the matched `se`.
- `pse_is_idle` — Whether the waking task's matched scheduling entity is idle. Recorded at the same point. Obtained via `se_is_idle(pse)` on the matched `pse`.
- `need_resched` — Whether `TIF_NEED_RESCHED` is set on `curr` after the function returns. Read via `test_tsk_need_resched(curr)`. This can be read in-place at the function exit.
- `need_resched_before` — Whether `TIF_NEED_RESCHED` was already set on `curr` before `check_preempt_wakeup_fair` was entered. Snapshot taken at function entry via `test_tsk_need_resched(curr)`. Needed to distinguish preemption set by this function vs. already-pending preemption.

**Check(s):**

Check 1: Performed at exit of `check_preempt_wakeup_fair`. Precondition: `need_resched_before` is false (no prior pending resched), and both `se` and `pse` are valid after `find_matching_se`.
```c
// If curr's matched entity is idle and waking task's is non-idle,
// preemption MUST be granted.
if (!need_resched_before && cse_is_idle && !pse_is_idle) {
    WARN_ON_ONCE(!test_tsk_need_resched(curr));
}
```

Check 2: Performed at exit of `check_preempt_wakeup_fair`. Same preconditions.
```c
// If waking task's matched entity is idle and curr's is non-idle,
// this function must NOT grant preemption.
if (!need_resched_before && !cse_is_idle && pse_is_idle) {
    WARN_ON_ONCE(test_tsk_need_resched(curr));
}
```

**Example violation:** The buggy code checks `task_has_idle_policy(curr)` before `find_matching_se()`, so a SCHED_NORMAL task in an idle cgroup preempts a SCHED_IDLE task in a normal cgroup. Check 1 fires because the matched curr entity is idle (idle cgroup) and the waking entity is non-idle (normal cgroup), yet `TIF_NEED_RESCHED` is not set due to the early return on `p->policy != SCHED_NORMAL`.

**Other bugs caught:** None known, but would catch any future regression that bypasses the cgroup hierarchy walk in the wakeup preemption path (e.g., adding early-exit checks based on raw task policy before `find_matching_se`).
