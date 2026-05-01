# Uclamp-Overutilized Consistency
**Source bug:** `c56ab1b3506ba0e7a872509964b100912bde165d`

**Property:** When uclamp is active, a CPU whose aggregated uclamp_max fits within its original capacity must not be considered overutilized; conversely, a non-max-capacity CPU whose aggregated uclamp_min exceeds its original capacity must be considered overutilized (when tasks are enqueued).

**Variables:**
- `rq_uclamp_min` — aggregated UCLAMP_MIN across all enqueued tasks on the rq. Read in-place at check time via `uclamp_rq_get(cpu_rq(cpu), UCLAMP_MIN)`.
- `rq_uclamp_max` — aggregated UCLAMP_MAX across all enqueued tasks on the rq. Read in-place at check time via `uclamp_rq_get(cpu_rq(cpu), UCLAMP_MAX)`.
- `cap_orig` — CPU's original capacity. Read in-place via `capacity_orig_of(cpu)`.
- `overutilized` — root domain overutilized flag. Read in-place via `READ_ONCE(cpu_rq(cpu)->rd->overutilized)`.
- `nr_running` — number of runnable tasks on the CPU. Read in-place via `cpu_rq(cpu)->nr_running`.

**Check(s):**

Check 1: Performed at `update_overutilized_status()` exit (or equivalently, after `task_tick_fair()` / `enqueue_task_fair()` calls it). Precondition: `uclamp_is_used() && nr_running > 0 && cap_orig < SCHED_CAPACITY_SCALE` (non-max-capacity CPU with uclamp active and tasks running).
```c
unsigned long rq_uclamp_max = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MAX);
unsigned long cap_orig = capacity_orig_of(cpu);

// If all tasks' effective utilization is capped within the CPU's
// original capacity, the CPU should NOT be overutilized.
if (rq_uclamp_max <= cap_orig &&
    rq_uclamp_max < SCHED_CAPACITY_SCALE) {
    WARN_ON(READ_ONCE(rq->rd->overutilized) == SG_OVERUTILIZED);
}
```

Check 2: Performed at `update_overutilized_status()` exit. Precondition: `uclamp_is_used() && nr_running > 0 && cap_orig < SCHED_CAPACITY_SCALE`.
```c
unsigned long rq_uclamp_min = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MIN);
unsigned long cap_orig = capacity_orig_of(cpu);
unsigned long util = cpu_util_cfs(cpu);

// If a task's uclamp_min demand exceeds the CPU's capacity and the
// actual utilization hasn't yet reached the demand (task is boosted),
// the CPU MUST be overutilized to trigger misfit migration.
if (rq_uclamp_min > cap_orig && util < rq_uclamp_min) {
    WARN_ON(READ_ONCE(rq->rd->overutilized) != SG_OVERUTILIZED);
}
```

**Example violation:** A task with `UCLAMP_MAX=512` runs on a CPU with `capacity_orig=512`. Its raw PELT utilization reaches ~512, causing `fits_capacity(512, 512)` to return false (due to the 20% margin), so the CPU is incorrectly marked overutilized — violating Check 1. Similarly, a task with `UCLAMP_MIN=1024` on that same CPU has low raw utilization, so `fits_capacity()` returns true and overutilized is never set — violating Check 2.

**Other bugs caught:** Potentially `uclamp_migration_margin_fits` and `uclamp_task_fits_migration_margin` (same series, same root cause of capacity-fitness checks ignoring uclamp).
