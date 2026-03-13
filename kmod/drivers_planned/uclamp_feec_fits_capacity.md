# Uclamp: fits_capacity() Migration Margin Breaks feec() CPU Selection

**Commit:** `244226035a1f9b2b6c326e55ae5188fab4f428cb`
**Affected files:** `kernel/sched/core.c`, `kernel/sched/fair.c`, `kernel/sched/sched.h`
**Fixed in:** v6.2-rc1
**Buggy since:** v5.6-rc1 (commit `1d42509e475c` "sched/fair: Make EAS wakeup placement consider uclamp restrictions")

## Bug Description

The `find_energy_efficient_cpu()` (feec) function in the scheduler's Energy Aware Scheduling (EAS) path incorrectly rejects all CPUs for tasks with a high `uclamp_min` value, causing them to always be placed on their previous CPU regardless of energy efficiency. Specifically, any task with `uclamp_min >= 820` (approximately 80% of `SCHED_CAPACITY_SCALE` = 1024) will never pass the `fits_capacity()` check inside feec(), because the 20% migration margin built into `fits_capacity()` makes even the biggest CPU appear insufficient.

The root issue is that the buggy code path in feec() first applies uclamp clamping via `uclamp_rq_util_with()`, which boosts the utilization value to at least `uclamp_min`, and then passes this boosted value to `fits_capacity()`. The `fits_capacity()` function checks whether `util * 1280 < capacity * 1024` (i.e., whether the utilization is within 80% of the CPU's capacity). When `uclamp_min` is high enough, the boosted utilization exceeds this 80% threshold on every CPU, causing feec() to skip all CPUs and fall back to `prev_cpu`.

This bug was reported by Yun Hsiang on the EAS development mailing list. The fix was authored by Qais Yousef as part of a 9-patch series ("Fix relationship between uclamp and fits_capacity()") that comprehensively addressed the broken interaction between uclamp values and the migration margin across multiple scheduler call sites. This particular commit (patch 3 of 9) fixes the feec() path specifically, introducing helper functions `uclamp_rq_get()`, `uclamp_rq_set()`, and `uclamp_rq_is_idle()` along the way.

Similar corner cases exist with `UCLAMP_MAX`. If a task is capped to `capacity_orig_of(medium_cpu)` via `uclamp_max`, it should fit on a medium CPU. But `fits_capacity()` with its migration margin may reject the medium CPU, pushing the task to a bigger CPU unnecessarily. The `util_fits_cpu()` function introduced in the companion patch (patch 1 of the series) resolves all these cases by separating the raw utilization check (which uses `capacity_of()` with the migration margin) from the uclamp constraint checks (which use `capacity_orig_of()` without the migration margin).

## Root Cause

The root cause is the misapplication of the `fits_capacity()` migration margin to uclamp-boosted utilization values in `find_energy_efficient_cpu()`.

The buggy code in `kernel/sched/fair.c` inside the per-CPU loop of feec() was:

```c
util = cpu_util_next(cpu, p, cpu);
cpu_cap = capacity_of(cpu);
util = uclamp_rq_util_with(cpu_rq(cpu), util, p);
if (!fits_capacity(util, cpu_cap))
    continue;
```

The function `uclamp_rq_util_with()` performs max-aggregation of the task's uclamp values with the runqueue's uclamp values, then clamps the raw utilization into the range `[max(task_uclamp_min, rq_uclamp_min), max(task_uclamp_max, rq_uclamp_max)]`. For a task with `uclamp_min = 1024` and a raw `util_avg = 300`, the result is `clamp(300, 1024, 1024) = 1024`.

The function `fits_capacity()` is defined as:

```c
static inline bool fits_capacity(unsigned long util, unsigned long capacity)
{
    return util * capacity_margin < capacity * SCHED_CAPACITY_SCALE;
}
```

where `capacity_margin = 1280` (1024 * 1.25). This 20% margin exists to speed up upmigration: a task at 80% of a CPU's capacity is considered "too big" and should migrate up to a more capable CPU.

The problem: when `uclamp_min >= 820`, `fits_capacity(820, 1024)` computes `820 * 1280 = 1,049,600` vs `1024 * 1024 = 1,048,576`. Since `1,049,600 >= 1,048,576`, the function returns false — the task "doesn't fit" even on the biggest CPU in the system (capacity 1024). For `uclamp_min = 1024` (a common boost value on Android), `fits_capacity(1024, 1024)` returns `1024 * 1280 = 1,310,720 >= 1,048,576 = false`.

Furthermore, `capacity_of(cpu)` already accounts for capacity pressure (irq time, thermal pressure, etc.), making it potentially less than `capacity_orig_of(cpu)`. Even the slightest IRQ pressure on the biggest CPU reduces its effective capacity below 1024, making the margin violation even worse. A task boosted to `uclamp_min = 800` could fail `fits_capacity()` if `capacity_of(biggest_cpu) = 980` due to IRQ pressure, because `800 * 1280 = 1,024,000 > 980 * 1024 = 1,003,520`.

The conceptual error is that the migration margin should only apply to the task's **actual** utilization (which represents real CPU demand), not to the **boosted** value (which represents a scheduling policy constraint). A task with `util_avg = 300` and `uclamp_min = 1024` is not actually consuming 1024 units of CPU time — it only consumes 300. The uclamp_min = 1024 is a performance hint saying "run this task at the highest frequency," not a declaration that the task needs 100% of CPU capacity. Applying the migration margin to the boosted value conflates frequency selection policy with actual workload demand.

Similarly, for `UCLAMP_MAX`, if a task is capped to `capacity_orig_of(medium_cpu)`, the 20% margin rejects the medium CPU, forcing the task to a bigger CPU — the opposite of what the cap intends.

## Consequence

The observable impact is incorrect CPU placement during EAS wakeup for tasks with high `uclamp_min` values on asymmetric CPU capacity platforms (ARM big.LITTLE, DynamIQ).

When the bug triggers, feec() finds no CPU that passes the `fits_capacity()` check, so it produces no better candidate than `prev_cpu`. The function returns `prev_cpu` as the target, regardless of whether that CPU is energy-efficient. In the worst case, a task that should be placed on a LITTLE CPU (because its actual utilization is low, even though its `uclamp_min` is high for frequency boosting) stays on the big CPU where it was previously running, wasting energy.

On Android devices, `uclamp_min` is actively used by the Android framework's performance management. Tasks in performance-critical paths (UI rendering, audio playback, touch event processing) commonly receive `uclamp_min` boosts of 800–1024 to ensure they run at high CPU frequencies. With this bug, all such tasks bypass EAS and stick to their previous CPU, negating EAS's energy-saving benefits for exactly the tasks that the platform most actively manages. This leads to measurable battery life regression on affected devices.

There is no crash, panic, or data corruption. The task runs correctly on the selected CPU — it is simply placed on a potentially suboptimal CPU from an energy perspective. However, since the affected tasks are typically UI-critical tasks with high uclamp boosts, the cumulative energy waste across all boosted tasks over time is significant.

## Fix Summary

The fix replaces the `fits_capacity(uclamp_rq_util_with(...), cpu_cap)` check with the new `util_fits_cpu(util, util_min, util_max, cpu)` function. The key insight is that uclamp constraints should be compared against `capacity_orig_of(cpu)` (the CPU's maximum capacity) **without** the migration margin, while the raw utilization should still be compared against `capacity_of(cpu)` **with** the migration margin.

The `util_fits_cpu()` function (introduced in patch 1 of the series) separates these concerns:

1. **Raw util check**: `fits = fits_capacity(util, capacity_of(cpu))` — applies migration margin to actual utilization.
2. **uclamp_max check**: `uclamp_max_fits = (uclamp_max <= capacity_orig_of(cpu))` — a capped task fits if the cap is within the CPU's original capacity. No migration margin applied. Exception: on the biggest CPU (capacity_orig == SCHED_CAPACITY_SCALE), capping to SCHED_CAPACITY_SCALE should not suppress the overutilized signal.
3. **uclamp_min check**: If `util < uclamp_min` (boosted), the boost must fit `capacity_orig_thermal` (capacity_orig minus thermal pressure). No migration margin applied.
4. The final result is `fits || uclamp_max_fits`, further refined by the boost check.

In the feec() code, the fix also open-codes `uclamp_rq_util_with()` to extract the **raw** (unclamped) uclamp_min and uclamp_max values. The `util_fits_cpu()` function needs these raw max-aggregated values rather than the final clamped utilization, because its logic must know whether the task is in the boosted region (util < uclamp_min), the capped region (util > uclamp_max), or the normal region (uclamp_min <= util <= uclamp_max). The open-coded aggregation uses the new `uclamp_rq_get()` helper to read the runqueue's uclamp values, and `uclamp_rq_is_idle()` to handle the idle-rq special case (where only the task's own uclamp values apply, not the rq's).

The fix also introduces three helper functions in `kernel/sched/sched.h`: `uclamp_rq_get(rq, clamp_id)` (read rq uclamp with `READ_ONCE`), `uclamp_rq_set(rq, clamp_id, value)` (write rq uclamp with `WRITE_ONCE`), and `uclamp_rq_is_idle(rq)` (check `UCLAMP_FLAG_IDLE`). These helpers replace scattered direct accesses to `rq->uclamp[clamp_id].value` across `core.c` and `sched.h`, improving readability and ensuring consistent use of atomic accessors.

## Triggering Conditions

The following conditions must ALL be met to trigger the bug:

- **Energy Aware Scheduling must be active**: EAS requires `CONFIG_ENERGY_MODEL=y`, `CONFIG_CPU_FREQ=y`, an asymmetric CPU capacity topology with `SD_ASYM_CPUCAPACITY`, registered Energy Model performance domains (`rd->pd != NULL`), and the `sched_energy_present` static key enabled. This means the system must be an ARM big.LITTLE or DynamIQ platform (or any platform with heterogeneous CPU capacities and a registered energy model).

- **Utilization clamping must be enabled and active**: `CONFIG_UCLAMP_TASK=y` must be set. The `sched_uclamp_used` static key must be enabled, which happens automatically when any task or cgroup sets a uclamp value.

- **A task must have a high effective `uclamp_min`**: Specifically, `uclamp_eff_value(p, UCLAMP_MIN)` must be >= ~820 (80% of SCHED_CAPACITY_SCALE). After max-aggregation with the runqueue's uclamp_min (`max(task_uclamp_min, rq_uclamp_min)`), the resulting `util_min` must be high enough that `fits_capacity(util_min, capacity_of(cpu))` returns false for all candidate CPUs. At the extreme, `uclamp_min = 1024` always triggers the bug.

- **The system must not be overutilized**: `rd->overutilized` must be false. If overutilized, feec() returns early before reaching the buggy check. This means total system utilization must be within total system capacity. A lightly loaded system with just the boosted task easily satisfies this.

- **The task must have non-negligible actual utilization**: `cpu_util_next(cpu, p, cpu)` must be positive for at least one candidate CPU. A task with zero utilization would produce `uclamp_rq_util_with() = uclamp_min`, which is the worst case for this bug.

- **The task must be woken up (not forked)**: feec() is called from `select_task_rq_fair()` during `SD_BALANCE_WAKE`. Forkees take the slow path via `find_idlest_cpu()`.

- **No race condition or timing sensitivity**: The bug is fully deterministic. Every wakeup of a task meeting the above conditions triggers the incorrect rejection of all CPUs by `fits_capacity()`.

- **At least 2 CPUs with different capacities**: The asymmetric topology requires at least one LITTLE and one big CPU. The bug is most apparent when the biggest CPU has `capacity_orig = SCHED_CAPACITY_SCALE = 1024`, as the threshold for triggering is exactly `uclamp_min >= ceil(0.8 * 1024) = 820` on the biggest CPU. On smaller CPUs, the threshold is proportionally lower.

## Reproduce Strategy (kSTEP)

Reproducing this bug requires activating the EAS path in `find_energy_efficient_cpu()` and observing incorrect CPU placement due to the `fits_capacity()` rejection. The approach is analogous to the one described for the `energy_feec_uclamp_max_zero` bug.

### Required kSTEP Extensions

1. **Energy Model Registration**: A helper function `kstep_em_register(cpumask, nr_states, power_table)` is needed to register fake energy model performance domains. This helper should:
   - Accept a CPU mask and a table of `(frequency_khz, power_mw)` pairs representing OPP states
   - Obtain the CPU device pointer via `get_cpu_device(cpu)` for the first CPU in the mask
   - Create an `em_data_callback` with an `active_power` callback returning power from the table
   - Call `em_dev_register_perf_domain()` (which is `EXPORT_SYMBOL_GPL` or accessible via `KSYM_IMPORT`)
   - This must be called BEFORE `kstep_topo_apply()` so that `build_perf_domains()` finds the registered EMs and creates `struct perf_domain` entries linked from `rd->pd`

2. **Kernel Configuration**: The kSTEP kernel must be built with `CONFIG_ENERGY_MODEL=y`, `CONFIG_CPU_FREQ=y`, `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y`, and `CONFIG_UCLAMP_TASK=y`. If these are not currently enabled, the kconfig needs updating.

3. **Uclamp API**: kSTEP tasks can already have uclamp values set via `sched_setattr_nocheck()` as demonstrated in the existing `uclamp_inversion.c` driver. No new API is needed for this part.

### Topology and Energy Model Setup (setup function)

1. **Configure 4 CPUs with asymmetric capacity**: Reserve CPU 0 for the driver. Set up two clusters:
   - CPUs 1-2: LITTLE cluster with `kstep_cpu_set_capacity(cpu, 512)` (half of SCHED_CAPACITY_SCALE)
   - CPUs 3-4: big cluster with `kstep_cpu_set_capacity(cpu, 1024)` (SCHED_CAPACITY_SCALE)

2. **Set up topology**: Use `kstep_topo_init()`, then `kstep_topo_set_cls()` with clusters `{"1-2", "3-4"}`, then `kstep_topo_apply()`.

3. **Register Energy Models** (before topo_apply):
   - LITTLE cluster (CPUs 1-2): 2 OPPs, e.g. (500000 kHz, 100 mW), (1000000 kHz, 300 mW)
   - big cluster (CPUs 3-4): 2 OPPs, e.g. (500000 kHz, 200 mW), (1000000 kHz, 800 mW)
   - The big cluster must consume more power per OPP than the LITTLE cluster, making LITTLE CPUs more energy-efficient for low-utilization tasks.

4. **Verify EAS is active**: After setup, check `this_rq()->rd->pd != NULL` and verify `sched_energy_present` static key is enabled (via `KSYM_IMPORT`). If perf domains are not populated, the EM registration or topology rebuild failed.

### Task Setup and Utilization Buildup

5. **Create the test task**: Use `kstep_task_create()` to create a CFS task.

6. **Pin to a big CPU and build utilization**: Pin the task to CPU 3 (big) using `kstep_task_pin(task, 3, 4)`. Wake it and let it run for ~200 ticks (`kstep_tick_repeat(200)`) to build up PELT utilization and `util_est`. After this, `task_util_est(task)` should be significantly nonzero (e.g., ~300–500). The task's `prev_cpu` will be CPU 3 (big).

7. **Set high uclamp_min**: Use `sched_setattr_nocheck()` to set `uclamp_min = 1024` (SCHED_CAPACITY_SCALE):
   ```c
   struct sched_attr attr = {
       .size = sizeof(attr),
       .sched_policy = SCHED_NORMAL,
       .sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
       .sched_util_min = 1024,
   };
   sched_setattr_nocheck(task, &attr);
   ```

### Triggering the Bug

8. **Block and unpin the task**: Use `kstep_task_block(task)` to dequeue the task, then remove the CPU pin (unpin or set affinity to all non-driver CPUs: 1-4) so that EAS has freedom to choose any CPU.

9. **Wake the task**: Use `kstep_task_wakeup(task)` to trigger the wakeup path: `select_task_rq_fair()` → `find_energy_efficient_cpu()`.

10. **Observe CPU placement**: After wakeup, check `task_cpu(task)`:
    - **Buggy kernel**: feec() calls `uclamp_rq_util_with()` which boosts util to 1024, then `fits_capacity(1024, capacity_of(cpu))` returns false for ALL CPUs (including big CPUs), so no CPU is selected. feec() returns `target = prev_cpu = 3` (big CPU). The task stays on the big CPU.
    - **Fixed kernel**: feec() calls `util_fits_cpu(util, 1024, 1024, cpu)`. The raw util (~300) easily passes `fits_capacity(300, capacity_of(big_cpu))`. The uclamp_min=1024 check passes because `1024 <= capacity_orig_of(big_cpu) = 1024`. For LITTLE CPUs, the raw util (~300) passes fits_capacity on LITTLE, but `uclamp_min=1024 > capacity_orig_of(LITTLE)=512` fails. So LITTLE CPUs are correctly rejected (cannot satisfy the boost), and big CPUs are correctly accepted. EAS computes energy and may place the task on either big CPU, whichever is more energy-efficient.

### Detection Logic (run function)

11. **Primary detection — CPU placement**:
    ```c
    int cpu = task_cpu(task);
    unsigned long cap = capacity_orig_of(cpu);
    if (cap >= 1024) {
        kstep_pass("Task placed on big CPU %d (cap=%lu) - EAS ran correctly", cpu, cap);
    } else {
        kstep_fail("Task placed on LITTLE CPU %d (cap=%lu) - unexpected", cpu, cap);
    }
    ```
    On the **buggy** kernel, the task should remain on CPU 3 (prev_cpu, big). On the **fixed** kernel, the task should also be placed on a big CPU (since uclamp_min=1024 requires a big CPU). The key difference is HOW the decision was made: on the buggy kernel, feec() selected no CPU and fell back to prev_cpu; on the fixed kernel, feec() correctly evaluated big CPUs as fitting and chose one based on energy computation.

12. **Alternative detection — test with a medium-capacity scenario**: To make the buggy vs fixed behavior more clearly distinguishable, use a 3-tier topology:
    - CPU 1: LITTLE, capacity = 341
    - CPU 2: medium, capacity = 682
    - CPU 3: big, capacity = 1024
    Set `uclamp_min = 600`. With `prev_cpu = 3` (big):
    - **Buggy**: `fits_capacity(600, capacity_of(cpu))` fails for LITTLE (600*1280 > 341*1024) and medium (600*1280=768000 > 682*1024=698368) and big (600*1280=768000 > 1024*1024=1048576, this actually passes). Wait — on the biggest CPU this would pass. So the bug is most pronounced when `uclamp_min >= 820`.

    Better scenario: Set `uclamp_min = 850`, `prev_cpu = 3` (big, capacity 1024):
    - **Buggy**: `uclamp_rq_util_with()` boosts util to 850. `fits_capacity(850, 1024)` → `850*1280 = 1,088,000 > 1,048,576` → false. All CPUs rejected. Falls back to prev_cpu (3).
    - **Fixed**: `util_fits_cpu(raw_util=300, 850, 1024, big_cpu)` → raw 300 fits big CPU capacity easily. `uclamp_min=850 <= capacity_orig=1024` → fits. Big CPU accepted. EAS computes energy normally.

    With `prev_cpu = 1` (LITTLE, capacity 512):
    - **Buggy**: Same rejection — no CPU fits. Falls back to prev_cpu = 1 (LITTLE), which cannot satisfy `uclamp_min = 850`. The task is stuck on an inadequate CPU.
    - **Fixed**: LITTLE rejected (uclamp_min=850 > capacity_orig=512). Big CPU accepted. Task migrates to big CPU.

    This second scenario (prev_cpu on LITTLE) provides the clearest distinction: buggy kernel keeps the task on LITTLE (wrong), fixed kernel migrates to big (correct).

13. **Robust detection approach**: Pin the task to a LITTLE CPU first to set prev_cpu = 1 (LITTLE), build utilization there, then unpin and set `uclamp_min = 850`:
    - Read `task_cpu(task)` after wakeup
    - **Buggy kernel**: `task_cpu(task) == 1` (LITTLE) — task stuck on prev_cpu
    - **Fixed kernel**: `task_cpu(task) == 3` or `4` (big) — task correctly migrated
    - Detection: `kstep_pass()` if task is on a big CPU, `kstep_fail()` if on LITTLE

### Expected Behavior Summary

| Scenario | Buggy Kernel | Fixed Kernel |
|---|---|---|
| uclamp_min=1024, prev_cpu=LITTLE | Stays on LITTLE (no CPU fits) | Migrates to big (uclamp_min fits big) |
| uclamp_min=850, prev_cpu=LITTLE | Stays on LITTLE (no CPU fits) | Migrates to big (uclamp_min fits big) |
| uclamp_min=850, prev_cpu=big | Stays on big (no CPU fits, falls back) | Stays on big (EAS correctly accepts big) |

### Kernel Version Guard

Guard the driver with:
```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
```
This matches the window from the introduction of the bug (commit `1d42509e475c` in v5.6-rc1) to the fix (this commit, merged before v6.2-rc1).

### QEMU Configuration

Configure QEMU with at least 5 CPUs (CPU 0 reserved for driver, CPUs 1-4 for the asymmetric topology). Standard memory (256M+) is sufficient.

### Caveats

- The Energy Model registration is the most complex part. If `em_dev_register_perf_domain()` is not directly callable from the module, use `KSYM_IMPORT(em_dev_register_perf_domain)` to obtain the function pointer.
- The `build_perf_domains()` function is called during sched domain rebuild (`partition_sched_domains_locked()`). The EM must be registered before this rebuild occurs. Since `kstep_topo_apply()` triggers the rebuild, register the EM first.
- Ensure the system is not overutilized by keeping utilization low. Only the test task should run on the non-driver CPUs.
- The `prev_cpu` value is critical. By pinning the task to a LITTLE CPU initially, then unpinning, we ensure `prev_cpu` is the LITTLE CPU, making the buggy/fixed behavior clearly distinguishable.
