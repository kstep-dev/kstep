# Cgroup uclamp_min Protection Floor

**Source bug:** `0c18f2ecfcc274a4bcc1d122f79ebd4001c3b445`

**Property:** For any task in a non-root, non-autogroup cgroup, the effective uclamp_min must be at least the task group's uclamp_min value (i.e., cgroup `cpu.uclamp.min` acts as a protection floor, not a limit ceiling).

**Variables:**
- `eff_min` — the task's effective uclamp_min value after restriction by the task group. Recorded at `uclamp_eff_get()` (called from `uclamp_cpu_inc()` during enqueue). Obtained as `uclamp_eff_value(p, UCLAMP_MIN)` or equivalently the return value of `uclamp_eff_get(p, UCLAMP_MIN).value`.
- `tg_min` — the task group's uclamp_min value. Read in-place from `task_group(p)->uclamp[UCLAMP_MIN].value` at the same point.
- `tg_max` — the task group's uclamp_max value. Read in-place from `task_group(p)->uclamp[UCLAMP_MAX].value`. Needed as a guard: when tg_max < tg_min, the effective min is legitimately clamped down (follow-up fix `0213b7083e81`).

**Check(s):**

Check 1: Performed at `uclamp_cpu_inc()` / `uclamp_eff_get()` return, when `clamp_id == UCLAMP_MIN`. Preconditions: task is in a non-root, non-autogroup cgroup, and CONFIG_UCLAMP_TASK_GROUP is enabled, and tg_min <= tg_max (no task-group-level inversion).
```c
// After uclamp_eff_get(p, UCLAMP_MIN) returns:
if (IS_ENABLED(CONFIG_UCLAMP_TASK_GROUP) &&
    !task_group_is_autogroup(task_group(p)) &&
    task_group(p) != &root_task_group) {
        unsigned int tg_min = task_group(p)->uclamp[UCLAMP_MIN].value;
        unsigned int tg_max = task_group(p)->uclamp[UCLAMP_MAX].value;
        unsigned int eff_min = uclamp_eff_value(p, UCLAMP_MIN);

        // Only check when TG values are themselves consistent
        if (tg_min <= tg_max)
                WARN_ON_ONCE(eff_min < tg_min);
}
```

**Example violation:** With the buggy code, a task requesting `uclamp_min = 50%` in a cgroup with `cpu.uclamp.min = 20%` gets effective uclamp_min capped to 20% (limit behavior). The invariant `eff_min >= tg_min` still holds in that case. But a task with `uclamp_min = 0` in the same cgroup gets effective uclamp_min = 0, violating `eff_min (0) >= tg_min (204)` — the protection floor was not applied.

**Other bugs caught:** Potentially `0213b7083e81f4acd69db32cb72eb4e5f220329a` (uclamp_tg_restrict_inversion) in the subset of cases where the inversion causes effective min to drop below the TG floor without a TG-level inversion.
