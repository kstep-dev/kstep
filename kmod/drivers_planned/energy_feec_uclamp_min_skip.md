# Energy: find_energy_efficient_cpu() Ignores uclamp_min Boost in Early Exit

**Commit:** `d81304bc6193554014d4372a01debdf65e1e9a4d`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.2-rc1
**Buggy since:** v5.0-rc1 (introduced by commit `732cd75b8c920` "sched/fair: Select an energy-efficient CPU on task wake-up")

## Bug Description

The `find_energy_efficient_cpu()` (feec) function in the Energy Aware Scheduling (EAS) wakeup path contains an early exit optimization that skips the energy calculation when the woken task's utilization is zero, under the rationale that a zero-utilization task has no impact on energy consumption and therefore EAS has nothing to optimize. However, this optimization fails to account for utilization clamping (`uclamp`). When a task has zero actual utilization (`task_util_est(p) == 0`) but is boosted via `uclamp_min` (e.g., `uclamp_min = 200`), the task will influence frequency selection and CPU placement decisions. The boosted utilization means the scheduler should consider energy implications of placing this task, but the early exit incorrectly skips the entire EAS computation.

The bug originates from the original `find_energy_efficient_cpu()` implementation in commit `732cd75b8c920`, which predates the introduction of comprehensive uclamp integration into the EAS path. At the time feec was written, utilization clamping was either not yet merged or not yet integrated into the energy-aware scheduling logic. The early exit check `if (!task_util_est(p)) goto unlock;` was a reasonable optimization in the absence of uclamp: a task with zero estimated utilization truly contributes nothing to energy. But once uclamp was introduced and tasks could be boosted above their actual utilization via `uclamp_min`, this check became incomplete.

The consequence is that on systems with EAS enabled (primarily ARM big.LITTLE and DynamIQ platforms), a task that has recently been idle (zero PELT utilization) but is boosted via `uclamp_min` will not receive energy-aware CPU placement. Instead, feec returns `prev_cpu` without computing energy, causing the task to stay on its previous CPU regardless of whether a different CPU would be more energy-efficient given the task's boosted utilization level.

This is part of a broader series of fixes by Qais Yousef (patch 7 of 9 in "[PATCH v2] Fix relationship between uclamp and fits_capacity()") that addressed multiple places where uclamp was not properly integrated into the scheduler's capacity and energy decision paths.

## Root Cause

The root cause is in `find_energy_efficient_cpu()` in `kernel/sched/fair.c`. The relevant code path (before the fix) is:

```c
static int find_energy_efficient_cpu(struct task_struct *p, int prev_cpu)
{
    unsigned long p_util_min = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MIN) : 0;
    unsigned long p_util_max = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MAX) : 1024;
    ...
    target = prev_cpu;

    sync_entity_load_avg(&p->se);
    if (!task_util_est(p))       /* <-- BUG: ignores uclamp_min boost */
        goto unlock;

    eenv_task_busy_time(&eenv, p, prev_cpu);
    ...
```

The function `task_util_est()` returns `max(task_util(p), _task_util_est(p))`, which is purely the PELT-based estimated utilization of the task. It does not consider uclamp clamping. When a task has been idle or has very low recent CPU usage, `task_util_est(p)` returns 0.

However, the task may have a non-zero `uclamp_min` effective value. The `uclamp_min` boost tells the scheduler that this task should be treated as if it has at least `uclamp_min` utilization for the purposes of frequency selection and capacity fitting. This means the task WILL affect energy consumption: placing it on a big CPU will cause the frequency governor to select a higher OPP than necessary if a LITTLE CPU at a lower OPP could satisfy the boost.

The early exit `if (!task_util_est(p)) goto unlock;` completely ignores the `p_util_min` and `p_util_max` values that are already computed at the top of the function. This is a logical inconsistency: feec computes the uclamp effective values but then makes its early-exit decision without consulting them.

The helper function `uclamp_task_util()` existed before this fix but was defined to take no uclamp parameters — it internally called `uclamp_eff_value()` itself:

```c
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
    return clamp(task_util_est(p),
                 uclamp_eff_value(p, UCLAMP_MIN),
                 uclamp_eff_value(p, UCLAMP_MAX));
}
```

Since feec already had `p_util_min` and `p_util_max` computed, calling the old `uclamp_task_util()` would redundantly recompute `uclamp_eff_value()`. The fix addresses both the missing uclamp check and this inefficiency in one change.

Consider the concrete scenario: a task `p` has `task_util_est(p) == 0` and `uclamp_eff_value(p, UCLAMP_MIN) == 512`. After clamping, the effective utilization is `clamp(0, 512, 1024) == 512`. This nonzero clamped utilization means the task will influence frequency selection: the schedutil governor will request a frequency corresponding to 512 utilization units. Placing this task on a big CPU versus a LITTLE CPU has different energy implications, and EAS should compute that. But the buggy code skips EAS because `task_util_est(p) == 0`.

## Consequence

The observable impact is suboptimal CPU placement for uclamp-boosted tasks with zero actual utilization on systems with Energy Aware Scheduling enabled. This primarily affects ARM big.LITTLE and DynamIQ platforms running Android, ChromeOS, or other EAS-enabled configurations.

When the bug triggers, `find_energy_efficient_cpu()` returns `prev_cpu` without performing any energy computation. The task is placed on whichever CPU it previously ran on, which may not be the most energy-efficient choice. For example, if a task was last running on a big CPU and then became idle (utilization decays to zero), but it has `uclamp_min = 512` (requesting at least medium performance), feec should ideally place it on a medium-capacity CPU or the most energy-efficient CPU that satisfies the boost. Instead, it stays on the big CPU, consuming more power than necessary. Conversely, if the task was last on a LITTLE CPU and is boosted to `uclamp_min = 1024`, EAS should migrate it to a big CPU for performance, but instead it stays on the LITTLE CPU, not receiving the performance boost it was promised.

The impact compounds across many such tasks. On Android devices, `uclamp_min` boosting is used extensively by the `SchedTune`/`UclampSetter` frameworks to ensure foreground tasks, top-app tasks, and latency-sensitive tasks receive adequate performance. When these tasks briefly become idle (e.g., between animation frames) and then wake up, their PELT utilization may be zero but their uclamp boost persists. Every such wakeup that hits the buggy early exit results in a potentially suboptimal placement decision. Over sustained workloads, this leads to measurable energy waste (tasks staying on power-hungry big CPUs) or performance degradation (boosted tasks stuck on LITTLE CPUs). There is no crash or kernel panic; this is strictly an energy efficiency and performance regression.

## Fix Summary

The fix makes two changes to `kernel/sched/fair.c`:

**1. Modify the `uclamp_task_util()` function signature** to accept pre-computed `uclamp_min` and `uclamp_max` parameters instead of internally calling `uclamp_eff_value()`:

```c
/* Before: */
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
    return clamp(task_util_est(p),
                 uclamp_eff_value(p, UCLAMP_MIN),
                 uclamp_eff_value(p, UCLAMP_MAX));
}

/* After: */
static inline unsigned long uclamp_task_util(struct task_struct *p,
                                             unsigned long uclamp_min,
                                             unsigned long uclamp_max)
{
    return clamp(task_util_est(p), uclamp_min, uclamp_max);
}
```

This avoids redundant `uclamp_eff_value()` calls since feec already computes `p_util_min` and `p_util_max` at the top of the function. The same change is made to the `#else` (non-CONFIG_UCLAMP_TASK) version of the function, which simply returns `task_util_est(p)` regardless of the parameters.

**2. Replace the early exit check** in `find_energy_efficient_cpu()`:

```c
/* Before: */
if (!task_util_est(p))
    goto unlock;

/* After: */
if (!uclamp_task_util(p, p_util_min, p_util_max))
    goto unlock;
```

Now the early exit only triggers when the *clamped* utilization is zero. If `task_util_est(p) == 0` but `uclamp_min > 0`, then `clamp(0, uclamp_min, uclamp_max)` returns `uclamp_min`, which is nonzero, so EAS proceeds with energy computation. This is the correct behavior: a boosted task should receive energy-aware placement even when its actual utilization is zero.

The fix is minimal and correct for the targeted scenario. It was reviewed and accepted by Peter Zijlstra. Note that this fix later required a follow-up fix (commit `23c9519def98`) because the `uclamp_task_util()` check introduced a complementary bug where `uclamp_max == 0` also causes EAS to be skipped — but that is a separate issue.

## Triggering Conditions

The following conditions must ALL be met simultaneously to trigger the bug:

- **Energy Aware Scheduling must be active**: This requires `CONFIG_ENERGY_MODEL=y`, `CONFIG_CPU_FREQ=y`, an asymmetric CPU capacity topology (big.LITTLE or DynamIQ with different CPU capacities), a registered Energy Model with performance domains, and the `sched_energy_present` static key enabled. The `rd->pd` linked list in the root domain must be non-NULL. The system must NOT be overutilized (`rd->overutilized == false`), otherwise feec exits even before the buggy check.

- **Utilization clamping must be enabled and in use**: `CONFIG_UCLAMP_TASK=y` must be set. The `uclamp_is_used()` static branch must be enabled, which happens automatically when any task or cgroup sets a uclamp value. Without uclamp, `p_util_min` defaults to 0, and the bug has no practical impact (a task with zero utilization and zero boost correctly has zero effective utilization).

- **A task must have non-zero effective uclamp_min**: `uclamp_eff_value(p, UCLAMP_MIN) > 0`. This can be set per-task via `sched_setattr()` with `SCHED_FLAG_UTIL_CLAMP_MIN`, or via cgroup `cpu.uclamp.min > 0`, or via the system-wide `/proc/sys/kernel/sched_util_clamp_min_rt_default` for RT tasks. The effective value considers the hierarchy of task requests, cgroup limits, and system-wide maximums.

- **The task must have zero estimated utilization**: `task_util_est(p) == 0`. This means `max(p->se.avg.util_avg, max(p->se.avg.util_est.ewma, p->se.avg.util_est.enqueued & ~UTIL_AVG_UNCHANGED)) == 0`. This occurs for newly created tasks that have never run, or for tasks that have been idle long enough for their PELT utilization to fully decay (which takes ~345ms with the default PELT half-life of 32ms).

- **The task must be woken up via the CFS wake path**: `find_energy_efficient_cpu()` is called from `select_task_rq_fair()` during `SD_BALANCE_WAKE` operations. This is the normal CFS task wakeup path triggered by `try_to_wake_up()`.

- **The task's prev_cpu must be within an EAS-capable sched_domain**: The `sd_asym_cpucapacity` scheduling domain must exist and span `prev_cpu`. If the topology doesn't include `prev_cpu` in an asymmetric domain, feec exits before reaching the buggy check.

- **No race conditions or timing sensitivity**: The bug is deterministic. Every single wakeup of a task meeting the above conditions will trigger the premature early exit. There is no concurrency requirement or probabilistic element.

- **Number of CPUs**: At least 2 CPUs with different capacities are needed. A typical affected system has 4-8 CPUs across 2-3 clusters (e.g., 4 LITTLE + 4 big, or 4 LITTLE + 3 medium + 1 big).

## Reproduce Strategy (kSTEP)

Reproducing this bug in kSTEP requires setting up a functional Energy Aware Scheduling environment, which needs minor extensions to the kSTEP framework. The strategy is similar to the approach described for the related commit `23c9519def98` (energy_feec_uclamp_max_zero) since both bugs are in the same early exit code path of `find_energy_efficient_cpu()`.

### Required kSTEP Extensions

1. **Energy Model Registration Helper**: Add a function `kstep_em_register(cpumask, nr_states, power_table)` or equivalent that calls `em_dev_register_perf_domain()` for a set of CPUs with synthetic performance states. The EM registration API (`em_dev_register_perf_domain`) is `EXPORT_SYMBOL_GPL` and callable from modules; alternatively, use `KSYM_IMPORT(em_dev_register_perf_domain)`. The helper needs:
   - A CPU device pointer obtained via `get_cpu_device(cpu)` for the first CPU in the mask
   - A `struct em_data_callback` with an `active_power` callback returning power from a lookup table
   - The number of performance states and the `(frequency, power)` pairs for each
   - The function is called once per performance domain (cluster)

2. **Kernel Configuration**: The kSTEP kernel config must include `CONFIG_ENERGY_MODEL=y` and `CONFIG_UCLAMP_TASK=y`. If these are not currently enabled, the kSTEP kernel `.config` needs updating. `CONFIG_CPU_FREQ=y` should also be present.

3. **uclamp Task Attribute Setting**: kSTEP needs a way to set per-task uclamp values. This can be done via `KSYM_IMPORT(sched_setattr_nocheck)` and constructing a `struct sched_attr` with `sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN` and `sched_util_min = <desired_value>`. This approach is already used by existing kSTEP drivers.

### Topology and EM Setup (setup callback)

4. **Configure QEMU with 5+ CPUs**: CPU 0 is reserved for the driver. CPUs 1-2 form a LITTLE cluster. CPUs 3-4 form a big cluster.

5. **Set asymmetric capacity**:
   ```c
   kstep_cpu_set_capacity(1, 512);  /* LITTLE */
   kstep_cpu_set_capacity(2, 512);  /* LITTLE */
   kstep_cpu_set_capacity(3, 1024); /* big */
   kstep_cpu_set_capacity(4, 1024); /* big */
   ```

6. **Set up topology**:
   ```c
   kstep_topo_init();
   const char *mc[] = {"1-2", "3-4"};
   kstep_topo_set_mc(mc, 2);
   ```

7. **Register Energy Models** (before `kstep_topo_apply()`):
   - LITTLE cluster (CPUs 1-2): 2 OPPs, e.g., (500MHz, 100mW), (1000MHz, 300mW)
   - big cluster (CPUs 3-4): 2 OPPs, e.g., (500MHz, 200mW), (1000MHz, 800mW)
   - The critical property is that the big cluster uses more power per OPP, so EAS has a reason to prefer LITTLE CPUs for low-utilization tasks.

8. **Apply topology**: `kstep_topo_apply()` triggers sched domain rebuild, which calls `build_perf_domains()`. With the registered EMs and asymmetric capacity, this should create `struct perf_domain` entries linked from `rd->pd`, enabling EAS.

9. **Verify EAS activation**: After setup, check that `this_rq()->rd->pd != NULL` and `sched_energy_present` is enabled (use `KSYM_IMPORT`). If EAS is not active, the driver should fail with a diagnostic message.

### Task Setup and Bug Triggering (run callback)

10. **Create the test task**: Use `kstep_task_create()` to create a CFS task. Pin it to CPU 3 (big) initially with `kstep_task_pin(task, 3, 4)` so `prev_cpu = 3`.

11. **Ensure zero utilization**: The freshly created task should already have `task_util_est(p) == 0`. If not (e.g., if the task ran briefly during creation), wait for PELT decay by calling `kstep_tick_repeat()` with enough ticks to fully decay utilization. A fresh task that has never been enqueued should have zero utilization.

12. **Set uclamp_min boost**: Use `KSYM_IMPORT(sched_setattr_nocheck)` to set `uclamp_min = 512` on the task:
    ```c
    struct sched_attr attr = {
        .size = sizeof(attr),
        .sched_policy = SCHED_NORMAL,
        .sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
        .sched_util_min = 512,
    };
    sched_setattr_nocheck(task, &attr);
    ```

13. **Block and re-wake**: Call `kstep_task_block(task)` to dequeue the task. Remove the CPU pin (or set it to allow all non-driver CPUs: `kstep_task_pin(task, 1, 5)`). Then call `kstep_task_wakeup(task)` to trigger the wakeup path → `select_task_rq_fair()` → `find_energy_efficient_cpu()`.

14. **Observe CPU placement**: After the wakeup, read `task_cpu(task)`:
    - **Buggy kernel**: `task_util_est(p)` is 0, so `!task_util_est(p)` is true, feec jumps to `unlock`, and returns `prev_cpu = 3` (big CPU). The task stays on the big CPU despite being a zero-utilization task that is boosted to 512 — EAS was never consulted.
    - **Fixed kernel**: `uclamp_task_util(p, 512, 1024)` returns `clamp(0, 512, 1024) = 512`, which is nonzero, so feec proceeds. EAS computes energy for all CPUs and, because the task's effective utilization (512) fits on the LITTLE cluster (capacity 512), and running on LITTLE saves energy compared to big, the task should be placed on CPU 1 or 2 (LITTLE).

### Detection Logic

15. **Primary pass/fail criterion**: Check `task_cpu(task)` after the wakeup:
    - If the task is on a LITTLE CPU (1 or 2): **PASS** — EAS ran and chose the energy-efficient placement.
    - If the task remains on the big CPU (3 or 4): **FAIL** — EAS was skipped due to the buggy early exit.

16. **Supplementary checks** (for robustness):
    - Before the wakeup, verify `task_util_est(task) == 0` (task has zero actual utilization) using `READ_ONCE(task->se.avg.util_est)` and `READ_ONCE(task->se.avg.util_avg)`.
    - Verify `uclamp_eff_value(task, UCLAMP_MIN) > 0` (task is boosted).
    - Verify `this_rq()->rd->pd != NULL` (EAS is active).
    - Verify `!READ_ONCE(this_rq()->rd->overutilized)` (system is not overutilized).
    - Log all these values with `TRACE_INFO()` for debugging.

17. **Alternative detection approach**: If CPU placement alone is unreliable (e.g., if other scheduling heuristics override EAS), an alternative is to directly check whether feec was entered and completed the energy loop. This can be done by:
    - Adding a `printk` or tracepoint probe near the early exit in the kernel source
    - Or by using `KSYM_IMPORT` to read the function's return value or internal state indirectly
    - Or by checking whether `eenv_task_busy_time()` was called (which happens after the early exit) by monitoring changes to the task's sched entity

### Kernel Version Guard

18. The driver should be guarded with:
    ```c
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0) && \
        LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
    ```
    The bug exists from v5.0-rc1 (when feec was introduced by `732cd75b8c920`) through v6.1 (inclusive). The fix was merged in v6.2-rc1. However, since kSTEP only supports v5.15+, the effective buggy range for testing is v5.15 through v6.1.

### Caveats

- **QEMU CPU count**: Configure QEMU with at least 5 CPUs (CPU 0 for driver, CPUs 1-4 for the two clusters).
- **CONFIG_ENERGY_MODEL**: Must be enabled in kSTEP's kernel build. If currently disabled, this is a prerequisite change.
- **EM registration ordering**: The Energy Model must be registered BEFORE `kstep_topo_apply()` triggers the sched domain rebuild, because `build_perf_domains()` checks for registered EMs when creating perf domains. If EM is not registered, `rd->pd` will be NULL and EAS won't activate.
- **Task utilization must be zero**: The test task must NOT have accumulated any PELT utilization. Creating a fresh task and immediately blocking it (before it gets enqueued on a runqueue) should ensure zero utilization. If the task briefly ran during creation, call `kstep_tick_repeat(100)` to allow PELT to decay.
- **System must not be overutilized**: Keep the system lightly loaded. Only the test task should be on the non-driver CPUs, and since its utilization is zero, the system should not be overutilized.
- **The asymmetric capacity domain must exist**: The `sd_asym_cpucapacity` pointer must be set on the waking CPU. This should happen automatically when `kstep_topo_apply()` builds sched domains with different-capacity CPUs.
