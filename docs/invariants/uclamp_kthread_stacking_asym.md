# select_idle_sibling Must Respect Asymmetric Capacity Fitness
**Source bug:** `014ba44e8184e1acf93e0cbb7089ee847802f8f0`

**Property:** On asymmetric CPU capacity systems, the CPU returned by `select_idle_sibling()` must have sufficient capacity for the task's uclamp-adjusted utilization.

**Variables:**
- `selected_cpu` — the CPU returned by `select_idle_sibling()`. Recorded at the return point of `select_idle_sibling()`. Read directly from the return value.
- `task_util` — the task's uclamp-clamped utilization, i.e., `uclamp_task_util(p)`. Recorded at the entry of `select_idle_sibling()` (already computed into local variable `task_util` when `sched_asym_cpucapacity` is set). Read in-place.
- `cpu_cap` — the capacity of `selected_cpu`, i.e., `capacity_of(selected_cpu)`. Read in-place at the check point.

**Check(s):**

Check 1: Performed at the return point of `select_idle_sibling()`. Only when `static_branch_unlikely(&sched_asym_cpucapacity)` is true.
```c
// At every return point of select_idle_sibling(), or equivalently
// after the function returns in select_task_rq_fair():
if (static_branch_unlikely(&sched_asym_cpucapacity)) {
    unsigned long util = uclamp_task_util(p);
    unsigned long cap = capacity_of(selected_cpu);
    WARN_ON_ONCE(!fits_capacity(util, cap));
}
```

Note: `fits_capacity(util, cap)` checks `(util * 1280) < (cap * 1024)`, i.e., utilization fits within ~80% of CPU capacity. This is the same margin used by `asym_fits_capacity()` throughout the scheduler.

**Example violation:** The per-CPU kthread stacking fast-path in `select_idle_sibling()` returned a LITTLE CPU (capacity 512) for a task with `uclamp.min = 800`, bypassing the `asym_fits_capacity()` check that all other early-return paths enforce. The invariant check at the return point would catch `fits_capacity(800, 512) == false`.

**Other bugs caught:** Potentially any future bug where a new early-return path is added to `select_idle_sibling()` without including the `asym_fits_capacity()` guard — a common pattern as this function has been extended multiple times. May also catch `uclamp_asym_fits_capacity` (related bug in the same subsystem).
