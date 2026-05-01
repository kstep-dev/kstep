# Uclamp-Capped Tasks Must Fit CPUs Matching Their Cap
**Source bug:** `b48e16a69792b5dc4a09d6807369d11b2970cc36`

**Property:** When a task's effective uclamp_max ≤ capacity_orig_of(cpu) and the task is the current running task on that CPU with nr_cpus_allowed > 1, then rq->misfit_task_load must be 0.

**Variables:**
- `uclamp_max` — the task's effective uclamp_max value. Recorded at `update_misfit_status()`. Obtained via `uclamp_eff_value(p, UCLAMP_MAX)`.
- `capacity_orig` — the CPU's original (maximum) capacity. Recorded at `update_misfit_status()`. Obtained via `capacity_orig_of(cpu_of(rq))`.
- `misfit_task_load` — the runqueue's misfit task load indicator. Read directly from `rq->misfit_task_load` after `update_misfit_status()` returns.
- `nr_cpus_allowed` — the task's CPU affinity width. Read from `p->nr_cpus_allowed`.

**Check(s):**

Check 1: Performed after `update_misfit_status()` returns (called from `task_tick_fair()` and `put_prev_entity()`). Preconditions: `sched_asym_cpucap_active()` is true, task `p` is non-NULL, and `p->nr_cpus_allowed > 1`.
```c
unsigned long uclamp_max = uclamp_eff_value(p, UCLAMP_MAX);
unsigned long cap_orig = capacity_orig_of(cpu_of(rq));

if (uclamp_max <= cap_orig && rq->misfit_task_load != 0) {
    // INVARIANT VIOLATED: task is capped at or below this CPU's
    // original capacity, yet is marked as misfit.
    WARN_ONCE(1, "misfit_task_load=%lu but uclamp_max=%lu <= cap_orig=%lu",
              rq->misfit_task_load, uclamp_max, cap_orig);
}
```

**Example violation:** A task with uclamp_max=512 runs on a CPU with capacity_orig=512. The buggy `task_fits_capacity()` applies a 20% migration margin, computing `fits_capacity(512, 512)` = `512*1280 < 512*1024` = false, so misfit_task_load is set non-zero despite the task being capped to exactly this CPU's capacity.

**Other bugs caught:**
- `48d5e9daa8b767e75ed9421665b037a49ce4bc04` (companion patch introducing util_fits_cpu — same migration margin vs uclamp issue)
- `244226035a1f9b2b6c326e55ae5188fab4f428cb` (feec() CPU selection applies margin to uclamp)
- `b759caa1d9f667b94727b2ad12589cbc4ce13a82` (select_idle_capacity ignores margin/uclamp interaction)
- `c56ab1b3506ba0e7a872509964b100912bde165d` (cpu_overutilized ignores uclamp constraints)
