# Uclamp Bounds Must Bypass Migration Margin in CPU Fitness Checks
**Source bug:** `a2e7f03ed28fce26c78b985f87913b6ce3accf9d`

**Property:** When evaluating whether a task fits a CPU for placement, uclamp bounds (uclamp_min, uclamp_max) must be compared against the CPU's original capacity without the fits_capacity() migration margin; only raw utilization should use the margin.

**Variables:**
- `task_util` — raw task utilization estimate (without uclamp clamping). Read from `task_util_est(p)` at the point of the fitness check.
- `util_min` — effective uclamp minimum of the task. Read from `uclamp_eff_value(p, UCLAMP_MIN)` at the fitness check point.
- `util_max` — effective uclamp maximum of the task. Read from `uclamp_eff_value(p, UCLAMP_MAX)` at the fitness check point.
- `cpu_capacity` — capacity_of(cpu), i.e., original capacity minus IRQ/thermal pressure. Read in-place at the fitness check.
- `cpu_capacity_orig` — capacity_orig_of(cpu), the CPU's nominal capacity. Read in-place at the fitness check.

**Check(s):**

Check 1: Performed at any CPU fitness evaluation used for task placement (e.g., `asym_fits_cpu()`, `task_fits_cpu()`, `select_idle_capacity()`, `find_energy_efficient_cpu()`). Precondition: `sched_asym_cpucap_active()` is true and task has non-default uclamp values.
```c
// If uclamp_max fits the CPU's original capacity (no margin),
// and raw utilization fits with margin, the CPU must not be rejected.
if (util_max <= capacity_orig_of(cpu) &&
    fits_capacity(task_util, capacity_of(cpu))) {
    // CPU fitness check MUST return true (task fits this CPU).
    // Returning false here indicates the migration margin was
    // incorrectly applied to uclamp bounds.
    WARN_ON_ONCE(!fitness_result);
}
```

Check 2: Performed at the same points. Validates uclamp_min is not subject to migration margin.
```c
// If uclamp_min fits the CPU's original capacity minus thermal pressure,
// the fitness check must not reject based on uclamp_min alone.
unsigned long capacity_orig_thermal = capacity_orig_of(cpu) - arch_scale_thermal_pressure(cpu);
if (util_min <= capacity_orig_thermal &&
    fits_capacity(task_util, capacity_of(cpu)) &&
    util_max <= capacity_orig_of(cpu)) {
    WARN_ON_ONCE(!fitness_result);
}
```

**Example violation:** A task with `uclamp_min = 1024` has `task_util = 100`. On a big CPU with `capacity_orig = 1024`, the buggy code computes `fits_capacity(1024, 1024)` → `1024*1280 < 1024*1024` → false, rejecting the CPU despite the task obviously fitting. The invariant catches this because `uclamp_min (1024) <= capacity_orig (1024)` and `fits_capacity(100, 1024)` = true, yet the fitness check returned false.

**Other bugs caught:**
- `48d5e9daa8b767e75ed9421665b037a49ce4bc04` (uclamp_migration_margin_fits — util_fits_cpu introduction)
- `b759caa1d9f667b94727b2ad12589cbc4ce13a82` (uclamp_select_idle_capacity_margin — select_idle_capacity)
- `244226035a1f9b2b6c326e55ae5188fab4f428cb` (uclamp_feec_fits_capacity — find_energy_efficient_cpu)
- `b48e16a69792b5dc4a09d6807369d11b2970cc36` (uclamp_task_fits_migration_margin — task_fits_capacity)
