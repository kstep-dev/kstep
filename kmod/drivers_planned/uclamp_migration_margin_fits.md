# Uclamp: Migration Margin Breaks Uclamp Capacity Fitting

**Commit:** `48d5e9daa8b767e75ed9421665b037a49ce4bc04`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.2-rc1 (merged via sched/core into v6.1-rc2 tag range)
**Buggy since:** v5.3-rc1 (introduced by commit af24bde8df202 "sched/uclamp: Add uclamp support to energy_compute()")

## Bug Description

The `fits_capacity()` macro used throughout the CFS scheduler applies a 20% migration margin when determining whether a task's utilization fits on a given CPU. The macro is defined as `#define fits_capacity(cap, max) ((cap) * 1280 < (max) * 1024)`, which means a task "fits" only if its utilization is less than approximately 80% of the CPU's capacity. This margin exists to speed up upmigration on asymmetric capacity systems (big.LITTLE): when a task on a LITTLE CPU reaches 80% of that CPU's capacity, it should migrate to a bigger CPU rather than waiting until it completely saturates.

However, when utilization clamping (uclamp) is used, this 20% margin creates several incorrect behaviors. The `uclamp_task_util()` function clamps the task's estimated utilization between `uclamp_min` and `uclamp_max`, and callers pass this clamped value directly to `fits_capacity()`. This is problematic because uclamp values represent target performance levels, not actual CPU utilization, and should not be subject to the migration margin.

For example, if a task is boosted to `uclamp_min = 1024` (SCHED_CAPACITY_SCALE), then `fits_capacity(1024, 1024)` evaluates to `1024 * 1280 < 1024 * 1024` → `1310720 < 1048576` → false. This means a task boosted to maximum performance does not "fit" on the biggest CPU in the system according to `fits_capacity()`. Similarly, if a task is boosted to `capacity_orig_of(medium_cpu)`, the 20% margin causes it to not fit on the medium CPU and instead migrate to a big CPU, defeating the purpose of the boost.

An analogous problem exists with `capacity_of()` vs `capacity_orig_of()`. The `capacity_of()` function returns a CPU's capacity reduced by RT/DL and IRQ pressure. When uclamp values are compared against `capacity_of()`, even slight IRQ pressure on the biggest CPU makes a 1024-boosted task appear unable to fit. Uclamp comparisons should use `capacity_orig_of()` (the raw hardware capacity) since uclamp specifies a target performance level, not an actual utilization level affected by pressure. The only exception is thermal pressure, which directly affects the available OPPs of the CPU.

## Root Cause

The root cause is that `fits_capacity()` was designed for comparing actual utilization signals against CPU capacity, where a margin is useful for proactive migration. But when uclamp was introduced, its clamped values were fed into the same `fits_capacity()` checks without accounting for the semantic difference between "actual utilization" and "target performance level."

There are several specific callers affected:

1. **`task_fits_capacity()`** (line 4429-4433): Calls `fits_capacity(uclamp_task_util(p), capacity)`, applying the 20% margin to the uclamp-clamped util value. Used by `update_misfit_status()` to decide if a task is a misfit on its current CPU.

2. **`cpu_overutilized()`** (line 5863-5866): Calls `fits_capacity(cpu_util_cfs(cpu), capacity_of(cpu))` which does not consider uclamp at all. A CPU running capped tasks (low `uclamp_max`) may appear overutilized based on raw CFS util even though the capped tasks should not trigger overutilization.

3. **`select_idle_capacity()`** (line 6655-6681): Uses `task_util = uclamp_task_util(p)` and then `fits_capacity(task_util, cpu_cap)`, applying the margin to the clamped value. This is used for idle CPU selection on asymmetric systems.

4. **`asym_fits_capacity()`** (line 6683-6688): Uses `fits_capacity(task_util, capacity_of(cpu))`, both applying the margin and using `capacity_of()` (with pressure) instead of `capacity_orig_of()` for the uclamp comparison.

5. **`find_energy_efficient_cpu()`** (line 7115-7117): Uses `util = uclamp_rq_util_with(cpu_rq(cpu), util, p)` followed by `fits_capacity(util, cpu_cap)`, applying the margin to uclamp-influenced aggregate utilization.

The correct behavior, as implemented by the new `util_fits_cpu()` function, is:
- Compare the **real** (unclamped) utilization against `capacity_of()` (with pressure) using the migration margin (`fits_capacity()`).
- Compare **uclamp_max** against `capacity_orig_of()` (without migration margin and without capacity pressure, except on the biggest CPU where overutilization should not be blocked).
- Compare **uclamp_min** against `capacity_orig_thermal` (capacity_orig minus thermal pressure, since thermal pressure affects available OPPs and may prevent delivering the requested minimum performance).

## Consequence

The consequences of this bug are observable on asymmetric capacity systems (big.LITTLE / DynamIQ) that use uclamp:

**Incorrect task placement**: A task boosted to `uclamp_min = 1024` cannot "fit" on any CPU in the system because `fits_capacity(1024, 1024)` is false even for the biggest CPU. This causes `select_idle_capacity()` to fall back to picking the CPU with maximum spare capacity rather than finding a fitting idle CPU. In `asym_fits_capacity()`, the task appears to not fit any CPU, causing the scheduler to skip preferred CPUs.

**Spurious overutilized state**: Because `task_fits_capacity()` with the margin sees boosted tasks as misfits, `update_misfit_status()` sets `rq->misfit_task_load` even when the task's actual utilization is low. This triggers misfit migration logic unnecessarily. Additionally, `cpu_overutilized()` not considering uclamp_max means CPUs running capped tasks can appear overutilized, blocking Energy Aware Scheduling (EAS) and forcing load balancing to take over.

**Energy inefficiency**: On EAS-enabled platforms, incorrect capacity fitting causes `find_energy_efficient_cpu()` to skip CPUs that should be candidates (because uclamp-influenced util doesn't "fit" with the margin), leading to suboptimal energy-aware placement. Tasks end up on bigger CPUs than necessary (wasting energy) or the overutilized flag disables EAS entirely, falling back to performance-oriented load balancing.

**Incorrect uclamp_max behavior**: A task capped to `uclamp_max = capacity_orig_of(medium_cpu)` should fit on a medium CPU without triggering overutilization. But `fits_capacity()` with its 20% margin requires the capped value to be less than 80% of the CPU capacity, so the task migrates to a big CPU unnecessarily, defeating the energy-saving intent of the cap.

## Fix Summary

This commit introduces a new `util_fits_cpu()` function in `kernel/sched/fair.c` that correctly handles the relationship between uclamp values and CPU capacity fitting. The function takes four parameters: `util` (actual task utilization), `uclamp_min`, `uclamp_max`, and `cpu`.

The function implements a three-part check:

1. **Real utilization check**: The actual `util` is compared against `capacity_of(cpu)` (which includes RT/DL and IRQ pressure) using the standard `fits_capacity()` margin. This preserves the existing proactive migration behavior for actual utilization.

2. **uclamp_max fitting**: The `uclamp_max` value is compared against `capacity_orig_of(cpu)` **without** the migration margin and **without** capacity pressure. The logic is: if `uclamp_max <= capacity_orig`, the task is forced to fit (the cap means we want the task on this or a smaller CPU). The exception is when `capacity_orig == SCHED_CAPACITY_SCALE` and `uclamp_max == SCHED_CAPACITY_SCALE` — on the biggest CPU with maximum cap, the overutilized state should not be blocked since there's no concept of capping at max capacity.

3. **uclamp_min fitting**: If `util < uclamp_min` (the task is being boosted beyond its actual utilization), the boosted value must fit the CPU. This comparison uses `capacity_orig_thermal` (capacity_orig minus thermal pressure, but not other pressures), since thermal pressure directly impacts available OPPs. The exception is on the biggest CPU (`capacity_orig == SCHED_CAPACITY_SCALE`), where the boost is allowed regardless because there's no bigger CPU to migrate to.

The function returns early with a simple `fits_capacity()` result if `uclamp_is_used()` returns false, ensuring zero overhead when uclamp is not active. This commit only introduces the function; subsequent patches in the series (patches 2-6 of 9) wire it into all the callers listed above.

## Triggering Conditions

- **Asymmetric CPU capacities required**: The system must have CPUs with different capacities (big.LITTLE or similar). On symmetric systems, `capacity_orig_of()` equals `SCHED_CAPACITY_SCALE` for all CPUs, and the special cases in `util_fits_cpu()` for the biggest CPU apply uniformly, making the bug less observable.

- **Uclamp must be active**: At least one task must have `uclamp_min > 0` or `uclamp_max < SCHED_CAPACITY_SCALE`. The bug only manifests when clamped utilization values are passed through `fits_capacity()`.

- **Specific uclamp values trigger the issue reliably**:
  - `uclamp_min = 1024` on any task: The task cannot fit on any CPU in the system via `fits_capacity(1024, capacity)` for any capacity ≤ 1024. This is the most extreme and easily reproducible case.
  - `uclamp_min = capacity_orig_of(medium_cpu)`: The task should fit on the medium CPU but the margin causes it to migrate to a big CPU.
  - `uclamp_max = capacity_orig_of(medium_cpu)` with `util < uclamp_max`: The task should be placed on the medium CPU but may not "fit" according to `fits_capacity()`.

- **The task must go through a wakeup path**: The buggy `fits_capacity()` checks are in `select_idle_capacity()`, `asym_fits_capacity()`, and `find_energy_efficient_cpu()`, which are called during `select_task_rq_fair()` on task wakeup. The `update_misfit_status()` path is called on each scheduler tick for the currently running task.

- **EAS may need to be active**: For the `find_energy_efficient_cpu()` path, EAS must be enabled (requires `sched_energy_present` to be true, typically through an energy model being registered). For the tick-based `update_misfit_status()` and `cpu_overutilized()` paths, EAS is not required — only `sched_asym_cpucap_active()` needs to return true.

- **No race conditions required**: This is a logic error, not a race. The bug triggers deterministically whenever the above conditions are met.

## Reproduce Strategy (kSTEP)

The bug can be reproduced using kSTEP by setting up an asymmetric CPU topology, creating a task with uclamp_min boost, and observing incorrect misfit task classification and/or overutilized state.

### Topology Setup

Configure a 4-CPU system simulating a big.LITTLE architecture:
- CPUs 0-1: LITTLE cluster with capacity 512 (scale 512)
- CPUs 2-3: big cluster with capacity 1024 (scale 1024)

Use `kstep_topo_init()`, then `kstep_topo_set_mc()` with two clusters `{"0-1", "2-3"}`, then `kstep_topo_apply()`. Set capacities with `kstep_cpu_set_capacity(0, 512)`, `kstep_cpu_set_capacity(1, 512)`, `kstep_cpu_set_capacity(2, 1024)`, `kstep_cpu_set_capacity(3, 1024)`. CPU 0 is reserved for the driver, so pin the test task to CPUs 1-3.

### Task Setup

1. Create a CFS task using `kstep_task_create()`.
2. Pin the task to CPUs 1-3 using `kstep_task_pin(p, 1, 3)`.
3. Set `uclamp_min = 1024` and `uclamp_max = 1024` using `sched_setattr_nocheck()` with `SCHED_FLAG_UTIL_CLAMP`, exactly as done in the existing `uclamp_inversion.c` driver.

### Reproduction Sequence

1. **Wake the task**: Call `kstep_task_wakeup(p)` to wake the task and trigger `select_task_rq_fair()`.
2. **Tick to build up scheduler state**: Call `kstep_tick_repeat(20)` to allow PELT signals to ramp and `update_misfit_status()` / `update_overutilized_status()` to be evaluated.
3. **Observe misfit_task_load**: After ticks, read `cpu_rq(task_cpu(p))->misfit_task_load`. On the buggy kernel, since `task_fits_capacity()` uses `fits_capacity(uclamp_task_util(p), capacity)` = `fits_capacity(1024, 1024)` which is false, `misfit_task_load` will be non-zero even on the biggest CPU. On the fixed kernel (after the full series is applied), `util_fits_cpu()` will correctly determine the task fits on the big CPU, so `misfit_task_load` will be 0.
4. **Observe overutilized state**: Read `cpu_rq(task_cpu(p))->rd->overutilized`. On the buggy kernel, the overutilized flag may be set spuriously. On the fixed kernel, it should remain clear for a low-actual-utilization task.

### Bug Detection Logic

In the `on_tick_end` callback:
1. Check `cpu_rq(task_cpu(p))->misfit_task_load`:
   - On a big CPU (capacity_orig == 1024), this should be 0 because the task fits the biggest CPU.
   - On the buggy kernel, it will be non-zero because `fits_capacity(1024, 1024)` returns false.
2. Alternatively, check `task_fits_capacity(p, capacity_of(task_cpu(p)))` directly by calling the function (via `KSYM_IMPORT` if needed) and comparing the result.
3. For explicit validation: compute `fits_capacity(1024, 1024)` manually (should be false on both buggy and fixed) and compare with the expected correct answer (should be true for a uclamp-boosted task on a big CPU).

### Pass/Fail Criteria

- **Buggy kernel (before commit)**: `misfit_task_load > 0` on the big CPU for a task with `uclamp_min = 1024` and low actual `util_avg`. Call `kstep_fail()` to record the bug.
- **Fixed kernel (after full series)**: `misfit_task_load == 0` on the big CPU for the same task. Call `kstep_pass()`.

### Important Notes

1. **This commit is patch 1 of 9**: This commit only introduces `util_fits_cpu()` but does not wire it into any callers. The behavioral fix requires subsequent patches in the same series (patches 2-6) that replace `fits_capacity()` calls with `util_fits_cpu()` in `task_fits_capacity()`, `select_idle_capacity()`, `asym_fits_capacity()`, `cpu_overutilized()`, and `find_energy_efficient_cpu()`. The buggy kernel should be checked out at the parent of this commit (`48d5e9daa8b7~1`), and the fixed kernel should include the full series. The last patch of the series that modifies behavioral callers should be identified and used as the fixed reference point.

2. **kSTEP already supports uclamp**: The existing `uclamp_inversion.c` driver demonstrates that `sched_setattr_nocheck()` with `SCHED_FLAG_UTIL_CLAMP` works in kSTEP. The `uclamp_task_util()` function and `uclamp_is_used()` static key will be active.

3. **kSTEP supports asymmetric capacities**: `kstep_cpu_set_capacity()` and `kstep_topo_*` APIs can create a big.LITTLE-like topology. The `sched_asym_cpucap_active()` flag should become true after topology setup, enabling the asymmetric capacity code paths.

4. **PELT warmup may be needed**: The task's `util_avg` starts at 0. After waking and running for several ticks, PELT will ramp up. For the reproduction, the actual `util_avg` should remain low (task does not run much) while `uclamp_min` is high. This creates the scenario where `uclamp_task_util()` returns the uclamp_min value (1024) but the actual utilization is low, and the task should fit on the big CPU.

5. **Alternative observation — `select_idle_capacity()` return value**: If a callback can be placed at the wakeup path, observe which CPU `select_idle_capacity()` returns. On the buggy kernel with `uclamp_min = 1024`, no CPU will pass `fits_capacity(1024, cpu_cap)` and the function falls back to `best_cpu` (maximum spare capacity). On the fixed kernel, the big CPU should be selected directly. This requires instrumenting the wakeup path, which may need kSTEP extensions (e.g., a `on_task_wakeup` callback).

6. **Guard with `#if LINUX_VERSION_CODE`**: Since the bug was introduced in v5.3-rc1 and fixed around v6.1-rc2/v6.2-rc1, the driver should be guarded for kernels in this range (approximately `KERNEL_VERSION(5, 3, 0)` to `KERNEL_VERSION(6, 1, 0)`). The `util_fits_cpu()` function only exists after this commit, so the buggy kernel check should rely on the observable behavior (misfit_task_load, overutilized).
