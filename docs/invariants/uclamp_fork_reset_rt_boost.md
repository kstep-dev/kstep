# Uclamp Request Consistency with Scheduling Policy
**Source bug:** `eaf5a92ebde5bca3bb2565616115bd6d579486cd`

**Property:** When a task's uclamp request is not user-defined, its value must equal the default for the task's current scheduling policy (0 for SCHED_NORMAL min, 1024 for RT min).

**Variables:**
- `p->uclamp_req[clamp_id].user_defined` — whether the user explicitly set this clamp via sched_setattr(). Read in-place from `task_struct`.
- `p->uclamp_req[clamp_id].value` — the current uclamp request value. Read in-place from `task_struct`.
- `p->policy` — the task's current scheduling policy. Read in-place from `task_struct`.
- `expected_default` — the expected default value for the clamp given the policy. Computed as: for `UCLAMP_MIN`, 1024 if `rt_task(p)` else 0; for `UCLAMP_MAX`, always 1024.

**Check(s):**

Check 1: Performed at exit of `sched_fork()`, after both `uclamp_fork()` and the reset-on-fork policy demotion have completed. Condition: `CONFIG_UCLAMP_TASK` is enabled.
```c
for_each_clamp_id(clamp_id) {
    if (!p->uclamp_req[clamp_id].user_defined) {
        unsigned int expected;
        if (clamp_id == UCLAMP_MIN)
            expected = rt_task(p) ? uclamp_none(UCLAMP_MAX) : uclamp_none(UCLAMP_MIN);
        else
            expected = uclamp_none(UCLAMP_MAX);
        WARN_ON_ONCE(p->uclamp_req[clamp_id].value != expected);
    }
}
```

Check 2: Performed at exit of `__setscheduler_uclamp()` or after any scheduling class change (`sched_setscheduler`, `switched_to_*`). Same condition.
```c
for_each_clamp_id(clamp_id) {
    if (!p->uclamp_req[clamp_id].user_defined) {
        unsigned int expected;
        if (clamp_id == UCLAMP_MIN)
            expected = rt_task(p) ? uclamp_none(UCLAMP_MAX) : uclamp_none(UCLAMP_MIN);
        else
            expected = uclamp_none(UCLAMP_MAX);
        WARN_ON_ONCE(p->uclamp_req[clamp_id].value != expected);
    }
}
```

**Example violation:** An RT task with `sched_reset_on_fork` forks a child. `uclamp_fork()` sets `uclamp_req[UCLAMP_MIN] = 1024` based on the still-RT policy, then the policy is demoted to `SCHED_NORMAL`. At `sched_fork()` exit, the task is `SCHED_NORMAL` with `user_defined=false` but `uclamp_req[UCLAMP_MIN].value = 1024` instead of the expected 0.

**Other bugs caught:** Potentially catches any future bug where a scheduling class transition fails to update non-user-defined uclamp defaults, including missing uclamp resets in `sched_setscheduler` paths or cgroup migration.
