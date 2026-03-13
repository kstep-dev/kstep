# PELT: util_est Boosting Causes Double-Counting of Waking Task

**Commit:** `c2e164ac33f75e0acb93004960c73bd9166d3d35`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.6-rc1
**Buggy since:** v6.5-rc1 (introduced by commit `7d0583cf9ec7` "sched/fair, cpufreq: Introduce 'runnable boosting'")

## Bug Description

The "runnable boosting" feature introduced in commit `7d0583cf9ec7` added a `boost` parameter to the `cpu_util()` function in `kernel/sched/fair.c`. When `boost=1`, `cpu_util()` replaces the CPU's `util_avg` with `max(util_avg, runnable_avg)` to detect CPU contention (where `runnable_avg > util_avg` indicates tasks are competing for CPU time). This boosting was also incorrectly applied to `util_est.enqueued`: `util_est = max(util_est, runnable_avg)`. The intent was to keep both the `util_avg` and `util_est` branches of `cpu_util()` in sync with the boosted value.

The fundamental problem is that `cfs_rq->avg.runnable_avg` and `cfs_rq->avg.util_est.enqueued` track different things. `runnable_avg` is a PELT-tracked signal that includes contributions from blocked (sleeping) tasks — it decays slowly after a task dequeues. In contrast, `util_est.enqueued` is updated discretely at enqueue/dequeue time and reflects only currently enqueued tasks. When `cpu_util()` is called during task wakeup with `boost=1`, `p != NULL`, and `dst_cpu == cpu`, boosting `util_est` to `runnable_avg` and then adding the waking task's `_task_util_est(p)` causes the waking task to be counted twice: once through its decaying contribution in `runnable_avg`, and again explicitly through `_task_util_est(p)`.

This double-counting inflates the CPU utilization estimate for the CPU where the task was previously running (`prev_cpu`), causing Energy Aware Scheduling (EAS) to incorrectly calculate a higher energy cost for keeping the task on `prev_cpu`. As a result, `find_energy_efficient_cpu()` (feec) selects a different CPU on the same performance domain, triggering an unnecessary migration. The task then bounces back when the target CPU's metrics catch up, creating a ping-pong migration pattern.

## Root Cause

The bug is in the `cpu_util()` function in `kernel/sched/fair.c`. When called with `boost=1` (as happens in the EAS path via `eenv_pd_max_util()` and `find_energy_efficient_cpu()`), the function executes the following sequence:

```c
if (boost) {
    runnable = READ_ONCE(cfs_rq->avg.runnable_avg);
    util = max(util, runnable);
}
```

This boosting of `util` (util_avg) is correct. `runnable_avg` and `util_avg` are both PELT-tracked signals that include the same set of task contributions (including blocked/decaying tasks). When a task's PELT contribution decays from `util_avg`, it also decays from `runnable_avg`, so there is no double-counting here.

The problem is in the `util_est` branch:

```c
util_est = READ_ONCE(cfs_rq->avg.util_est.enqueued);

if (boost)
    util_est = max(util_est, runnable);  // BUG: overwrites util_est with runnable_avg
```

After this boosting, `util_est` may now contain `runnable_avg`, which includes the decaying contribution of task `p` (which blocked recently and is now being woken up). Then, the code simulates what the CPU's utilization would look like after `p` is enqueued:

```c
if (dst_cpu == cpu)
    util_est += _task_util_est(p);  // Adds p's util_est AGAIN
```

Since `p` was recently running on this CPU and has only been blocked for a short time, `runnable_avg` still contains a significant contribution from `p`. By setting `util_est = runnable_avg` and then adding `_task_util_est(p)`, the function effectively counts task `p` twice.

Consider a concrete example from the mailing list discussion. Task A has `util_avg=200`, `util_est=300`, `runnable=200`. It was running on CPU0, so CPU0 has `util_avg=200`, `util_est.enqueued=0` (task dequeued for wakeup), `runnable_avg=200`. CPU1 is idle with all zeros. During wakeup, `cpu_util()` is called for both CPUs:

- **CPU0 (prev_cpu):** `util_est = max(0, 200) = 200`, then `util_est += 300 = 500`. Combined: `max(200, 500) = 500`.
- **CPU1 (target):** `util_est = max(0, 0) = 0`, then `util_est += 300 = 300`. Combined: `max(0, 300) = 300`.

This 200-point difference (500 vs 300) is entirely due to double-counting. Without the boost, both CPUs would compute `max(util_avg ± task_util, 0 + 300) = 300`, showing equal util_est and favoring `prev_cpu` per the tie-breaking logic.

The for-`util_avg` case does not have this problem because it correctly adjusts for task migration:

```c
if (p && task_cpu(p) == cpu && dst_cpu != cpu)
    lsub_positive(&util, task_util(p));  // Remove p's util contribution
else if (p && task_cpu(p) != cpu && dst_cpu == cpu)
    util += task_util(p);  // Add p's util contribution
```

This adjustment removes or adds the task's PELT contribution to avoid double-counting. No such adjustment exists after boosting `util_est` with `runnable_avg`.

## Consequence

The primary consequence is excessive and unnecessary task migrations on systems with Energy Aware Scheduling (EAS) enabled — typically mobile devices with asymmetric CPU topologies (big.LITTLE / DynamIQ). When `eenv_pd_max_util()` computes the maximum utilization for each CPU in a performance domain, the inflated `cpu_util()` value for `prev_cpu` causes the EAS energy model to select a higher Operating Performance Point (OPP) for `prev_cpu`. This makes `prev_cpu` appear more energy-expensive than an alternative CPU in the same performance domain.

The `find_energy_efficient_cpu()` function compares the energy cost of the `prev_cpu` placement against the `best_cpu` candidate. The condition `if (max_spare_cap_cpu >= 0 && max_spare_cap > prev_spare_cap)` is intended to prefer `prev_cpu` when spare capacities are equal. However, small background activities (cpufreq, timers, IRQs, RCU) on `prev_cpu` can create a tiny spare capacity deficit (often just 1 unit), bypassing this filter. Combined with the inflated utilization from double-counting, feec migrates the task to the alternative CPU.

Once the task migrates to the new CPU, that CPU's metrics increase while `prev_cpu`'s metrics decay. At the next wakeup, the situation reverses, and the task migrates back. Vincent Guittot's testing on a Qualcomm Snapdragon RB5 demonstrated this dramatically: running 3 tasks (1 medium, 2 small) for 30 seconds on the buggy kernel produced **3665 migrations**, while the fixed kernel produced only **8 migrations** (initial placement). Dietmar Eggemann's Jankbench tests on Pixel 6 showed the fix maintained the same jank improvement (−69% jank percentage) while reducing power consumption from 134.3 mW (with runnable boosting) to 129.9 mW (essentially matching the 129.5 mW baseline without any boosting), demonstrating that the util_est boosting was purely harmful with no compensating benefit.

## Fix Summary

The fix is a simple 3-line deletion that removes the boosting of `util_est` with `runnable_avg`:

```c
-		if (boost)
-			util_est = max(util_est, runnable);
```

After this change, `util_est` retains its original value from `cfs_rq->avg.util_est.enqueued` without being overridden by `runnable_avg`. The boosting of `util` (util_avg) is preserved — `util = max(util_avg, runnable_avg)` when `boost=1` — which is correct because both `util_avg` and `runnable_avg` are PELT signals that track the same set of task contributions and undergo the same migration adjustments.

The key insight is that `util_est` does not need separate boosting because the final result of `cpu_util()` is `max(util, util_est)`. Since `util` is already boosted with `runnable_avg`, and `util_est` represents a complementary estimate (preserving peak utilization snapshots), the maximum of the two is sufficient. There is no scenario where `util_est` being lower than `runnable_avg` causes a problem, because `util` itself is already at least `runnable_avg`. The original motivation for boosting `util_est` was to ensure `max(util, util_est)` remained boosted, but this was unnecessary since `util` was already boosted.

This fix is correct and complete because it eliminates the only source of double-counting (the util_est boost) while preserving the beneficial util_avg boost that provides the CPU contention detection (runnable > utilization) needed by schedutil and EAS for responsive frequency scaling.

## Triggering Conditions

The bug requires all of the following conditions to be met simultaneously:

1. **EAS must be active**: The system must have `sched_energy_present == true`, which requires an asymmetric CPU topology with an Energy Model registered via `em_dev_register_perf_domain()`, frequency invariance via `arch_scale_freq_invariant()`, and a schedutil-compatible cpufreq governor. This is typical on mobile devices (ARM big.LITTLE / DynamIQ) but not on symmetric x86 systems.

2. **A CFS task must be waking up** (not already running): The bug triggers in `select_task_rq_fair()` → `find_energy_efficient_cpu()` during the task wakeup path. The task must have been recently blocked so that `cfs_rq->avg.runnable_avg` still contains a significant decaying contribution from the task.

3. **The task must have non-trivial `util_est`**: The task's `_task_util_est(p)` must be significant (i.e., the task has run enough in the past to build up a util_est snapshot). This is common for periodic tasks like UI rendering threads.

4. **`runnable_avg` must differ from `util_est.enqueued`**: After the task dequeues, `util_est_dequeue()` removes the task's contribution from `cfs_rq->avg.util_est.enqueued`. But `runnable_avg` still contains the decaying PELT contribution. If `runnable_avg > util_est.enqueued`, the boosting replaces `util_est` with `runnable_avg`, enabling the double-count.

5. **Multiple CPUs in the same performance domain**: There must be at least two CPUs in the same performance domain (same capacity, same OPP table) so that `eenv_pd_max_util()` can compare them and potentially prefer a different CPU over `prev_cpu`.

6. **Small asymmetry in background activity**: Small background activities (cpufreq, timers, IRQs) on `prev_cpu` create a tiny spare capacity deficit that prevents the `max_spare_cap > prev_spare_cap` guard from protecting `prev_cpu`. Even a 1-unit difference is sufficient.

The bug is highly reproducible on EAS-enabled systems with periodic tasks. It does not require a race condition — it is a deterministic logic error that triggers on every wakeup when the above conditions are met.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why kSTEP Cannot Reproduce This Bug

The bug manifests exclusively through Energy Aware Scheduling (EAS) code paths. Specifically, the buggy `util_est = max(util_est, runnable)` line in `cpu_util()` is only reached when `boost=1` AND `p != NULL` AND `dst_cpu == cpu`. The only callers that satisfy all three conditions are:

- `eenv_pd_max_util()` — called from `compute_energy()` within `find_energy_efficient_cpu()`
- `find_energy_efficient_cpu()` — called from `select_task_rq_fair()` when `sched_energy_present == true`

The function `cpu_util_cfs_boost()`, which also passes `boost=1`, calls `cpu_util(cpu, NULL, -1, 1)` with `p = NULL`, so the double-counting branch is never reached.

EAS activation requires three fundamental prerequisites that QEMU cannot provide:

**a) Energy Model**: EAS requires a registered Energy Model (`em_dev_register_perf_domain()`) that maps CPU performance states to power consumption. This is normally registered by cpufreq drivers (e.g., `cpufreq-dt`, `scmi-cpufreq`) based on device tree or firmware data. QEMU has no cpufreq hardware or driver, so no Energy Model can be registered.

**b) Frequency Invariance**: `build_perf_domains()` checks `arch_scale_freq_invariant()` before enabling EAS. On x86 (which kSTEP/QEMU uses), this requires the `arch_scale_freq_key` static key to be enabled, which is done by cpufreq drivers like `intel_pstate`. QEMU's virtual CPU has no frequency scaling capability, so this invariance flag is never set.

**c) Asymmetric CPU Capacities**: While kSTEP can set CPU capacities via `kstep_cpu_set_capacity()`, without a functioning cpufreq driver and Energy Model, the `SD_ASYM_CPUCAPACITY` flag alone is insufficient to activate EAS — `sched_energy_present` remains false.

Without `sched_energy_present == true`, `find_energy_efficient_cpu()` returns -1 immediately, and the buggy code path is never executed.

### 2. What Would Need to Change in kSTEP

To support this bug, kSTEP would need a comprehensive EAS enablement mechanism:

- **`kstep_register_energy_model(cpu_mask, opp_table, nr_opps)`**: A new API to register a fake Energy Model for a set of CPUs. This would call `em_dev_register_perf_domain()` with a synthetic OPP/power table and the CPU's `struct device *` (obtained via `get_cpu_device()`).

- **`kstep_enable_freq_invariance()`**: A mechanism to enable `arch_scale_freq_invariant()`. On x86, this would need to enable the `arch_scale_freq_key` static key. This is fragile because the kernel expects this to be managed by cpufreq infrastructure.

- **`kstep_rebuild_perf_domains()`**: After registering the energy model and enabling invariance, a sched domain rebuild would be needed to trigger `build_perf_domains()` and set `sched_energy_present = true`.

These changes go beyond a "minor extension" because they require faking multiple interconnected kernel subsystems (cpufreq, energy model, frequency invariance) that normally require real hardware backing. The resulting setup would be fragile and could cause kernel warnings or crashes from other subsystems that expect consistent cpufreq state.

### 3. Alternative Reproduction Methods

**On real ARM hardware** (preferred): Use a device with EAS support (e.g., Qualcomm Snapdragon, Samsung Exynos, or Google Tensor). Create 2-3 periodic CFS tasks of varying utilization sizes. Use `trace-cmd` or `ftrace` with the `sched_migrate_task` tracepoint to count task migrations over 30 seconds. On the buggy kernel (v6.5-rc1 through v6.5), expect thousands of migrations; on the fixed kernel, expect single-digit migrations.

**Using QEMU with ARM emulation**: kSTEP could potentially be adapted to use an ARM64 QEMU target with a device tree that registers a `cpufreq-dt` driver and energy model. This would be a major architectural change to kSTEP.

**Analytical verification**: While kSTEP cannot trigger the bug, it could verify the underlying PELT double-counting condition by creating a task, letting it build util_est, blocking it, then reading `cfs_rq->avg.runnable_avg`, `cfs_rq->avg.util_est.enqueued`, and `_task_util_est(p)` to confirm the arithmetic conditions for double-counting are met. However, this only validates the precondition, not the actual scheduling misbehavior.
