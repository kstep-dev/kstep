# Uclamp-Boosted Task Must Fit Biggest CPU
**Source bug:** `48d5e9daa8b767e75ed9421665b037a49ce4bc04`

**Property:** On the biggest CPU (capacity_orig == SCHED_CAPACITY_SCALE), a task whose actual utilization fits the CPU must not be marked as a misfit task, regardless of uclamp boost values.

**Variables:**
- `misfit_task_load` — the misfit load recorded for the running task on its CPU. Read in-place from `cpu_rq(cpu)->misfit_task_load` after `update_misfit_status()` runs (called from `task_tick_fair()`).
- `util_avg` — the task's actual PELT utilization. Read in-place from `p->se.avg.util_avg`.
- `capacity_orig` — the original (hardware) capacity of the CPU. Read from `capacity_orig_of(cpu)`.
- `capacity` — the effective capacity of the CPU (after RT/DL/IRQ pressure). Read from `capacity_of(cpu)`.

**Check(s):**

Check 1: Performed at `update_misfit_status()` exit (or equivalently in `task_tick_fair()` after misfit update). Precondition: `sched_asym_cpucap_active()` is true, the task is CFS, and the CPU is the biggest (capacity_orig == SCHED_CAPACITY_SCALE).
```c
// After update_misfit_status() runs for task p on cpu:
if (capacity_orig_of(cpu) == SCHED_CAPACITY_SCALE &&
    fits_capacity(p->se.avg.util_avg, capacity_of(cpu))) {
	// The task's real utilization fits. Misfit must be 0,
	// regardless of uclamp_min or uclamp_max values.
	WARN_ON_ONCE(cpu_rq(cpu)->misfit_task_load > 0);
}
```

**Example violation:** A task with `util_avg = 300` and `uclamp_min = 1024` runs on a big CPU (capacity 1024). The buggy code passes `uclamp_task_util(p) = 1024` to `fits_capacity(1024, 1024)`, which returns false due to the 20% margin, causing `misfit_task_load` to be set non-zero even though the task's real utilization (300) easily fits.

**Other bugs caught:** The same invariant would detect the behavioral manifestation of the entire patch series: `uclamp_task_fits_migration_margin`, `uclamp_select_idle_capacity_margin`, `uclamp_cpu_overutilized_unaware`, `uclamp_feec_fits_capacity`, `uclamp_asym_fits_capacity` — all of which stem from applying the migration margin to uclamp-clamped values.
