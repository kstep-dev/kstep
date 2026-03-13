# Energy: EAS Skips CPUs With Zero Spare Capacity Under uclamp_max

**Commit:** `6b00a40147653c8ea748e8f4396510f252763364`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.7-rc1
**Buggy since:** v5.6-rc1 (introduced by commit `1d42509e475c` "sched/fair: Make EAS wakeup placement consider uclamp restrictions")

## Bug Description

The Energy Aware Scheduling (EAS) wakeup path in `find_energy_efficient_cpu()` fails to consider CPUs that have zero spare capacity when `uclamp_max` is being used to cap a task's utilization. When a task has high actual utilization but a low `uclamp_max` value, the `util_fits_cpu()` function correctly determines that the task fits on a given CPU due to the uclamp capping. However, after `lsub_positive(&cpu_cap, util)` subtracts the task's utilization from the CPU capacity, the resulting `cpu_cap` (spare capacity) can be zero. Since the EAS code uses zero as the initial sentinel value for `max_spare_cap` and `prev_spare_cap`, this creates an off-by-one style logic error where CPUs with zero spare capacity are silently ignored.

The core issue is that the variables `max_spare_cap` and `prev_spare_cap` are declared as `unsigned long` and initialized to 0. The value 0 is used both as a sentinel meaning "not yet populated" and as a legitimate spare capacity value. When a CPU's spare capacity is exactly 0 (which happens when the task's utilization matches the CPU capacity), the comparison `cpu_cap > max_spare_cap` evaluates to `0 > 0`, which is false, preventing that CPU from being selected as a candidate. Similarly, the check `prev_spare_cap == 0` used to skip entire performance domains fails to distinguish between "prev_cpu was never evaluated" and "prev_cpu was evaluated but has 0 spare capacity."

This bug specifically affects big.LITTLE or heterogeneous CPU topologies where EAS is active and tasks are capped with `uclamp_max`. The capped task is supposed to be placed on a smaller/more energy-efficient CPU even when the spare capacity there is zero, but instead the entire performance domain may be skipped, leading to suboptimal energy-aware placement decisions.

## Root Cause

In `find_energy_efficient_cpu()`, the per-performance-domain loop initializes two tracking variables:

```c
unsigned long max_spare_cap = 0;  /* best spare cap among non-prev CPUs */
unsigned long prev_spare_cap = 0; /* spare cap on prev_cpu */
```

For each CPU in the performance domain, after `util_fits_cpu()` confirms the task fits (potentially due to `uclamp_max` forcing the fit), the code computes spare capacity:

```c
lsub_positive(&cpu_cap, util);  /* cpu_cap = max(0, capacity_of(cpu) - util) */
```

When `util` equals `capacity_of(cpu)` — a common scenario when a high-utilization task is being force-fit onto a CPU via `uclamp_max` — `cpu_cap` becomes 0. The subsequent selection logic then fails:

For `prev_cpu`:
```c
if (cpu == prev_cpu) {
    prev_spare_cap = cpu_cap;  /* prev_spare_cap = 0 */
    prev_fits = fits;
}
```

For other CPUs:
```c
} else if ((fits > max_fits) ||
           ((fits == max_fits) && (cpu_cap > max_spare_cap))) {
    /* 0 > 0 is false, so this CPU is NEVER selected */
    max_spare_cap = cpu_cap;
    max_spare_cap_cpu = cpu;
    max_fits = fits;
}
```

After the per-CPU loop, the code decides whether to compute energy for this performance domain:

```c
if (max_spare_cap_cpu < 0 && prev_spare_cap == 0)
    continue;  /* Skips the entire PD! */
```

When `prev_spare_cap` was set to 0 (the legitimate value), this condition is `true` (since `max_spare_cap_cpu` was never set either), and the performance domain is entirely skipped. No `compute_energy()` call is made for it, meaning EAS misses the opportunity to evaluate this CPU cluster for energy-efficient placement.

Additionally, the `prev_cpu` energy evaluation check also fails:

```c
if (prev_spare_cap > 0) {  /* 0 > 0 is false, prev_cpu evaluation skipped */
    prev_delta = compute_energy(...);
    ...
}
```

This means even when `prev_cpu` has zero spare capacity but is a valid candidate (because `util_fits_cpu()` said the task fits thanks to uclamp_max), its energy cost is never computed.

The root cause is the conflation of "uninitialized" (0) with "valid zero spare capacity" (0) in the `unsigned long` type, which cannot represent a distinct sentinel value below 0.

## Consequence

The observable impact is suboptimal task placement on big.LITTLE or heterogeneous CPU systems that use `uclamp_max` to cap task utilization. Instead of placing a uclamp-capped task on the most energy-efficient CPU (which might be a small/LITTLE core with zero spare capacity), the scheduler may either:

1. **Skip entire performance domains**: If all CPUs in a cluster result in zero spare capacity and `prev_cpu` also has zero spare capacity in that cluster, the entire domain is skipped by the `continue` statement. This means `compute_energy()` is never called for that cluster, and a potentially optimal placement is missed.

2. **Fall back to suboptimal placement**: The task may end up on a big core or a less energy-efficient CPU because the small core cluster was not evaluated. This directly contradicts the purpose of using `uclamp_max` to constrain a task's power consumption, as the task gets placed on a higher-power CPU instead.

3. **Ignore prev_cpu as a candidate**: Even when `prev_cpu` is in the correct cluster and is a valid candidate, the `prev_spare_cap > 0` check prevents its energy from being computed. This can cause unnecessary task migrations away from `prev_cpu`.

The impact is primarily on power consumption and battery life for mobile/embedded systems (e.g., Android devices using big.LITTLE ARM SoCs) where EAS and uclamp are actively used for power management. There is no crash, hang, or data corruption — the consequence is purely a scheduling quality regression that results in higher energy consumption than necessary.

## Fix Summary

The fix changes `max_spare_cap` and `prev_spare_cap` from `unsigned long` initialized to 0 to `long` initialized to -1. This introduces a proper sentinel value that is distinct from any valid spare capacity (which is always >= 0). The specific changes are:

1. **Variable declarations**: `long prev_spare_cap = -1, max_spare_cap = -1;` replaces the two separate `unsigned long` declarations.

2. **Signed comparison for max_spare_cap selection**: The condition `cpu_cap > max_spare_cap` becomes `(long)cpu_cap > max_spare_cap`. The cast to `long` is necessary because `cpu_cap` remains `unsigned long` (as it comes from `capacity_of()` and `lsub_positive()`), and comparing an `unsigned long` with a `long` would use unsigned semantics, making -1 appear as a very large number. With the cast, a `cpu_cap` of 0 correctly compares as `0 > -1` which is `true`, so the CPU is selected.

3. **Performance domain skip condition**: `prev_spare_cap == 0` becomes `prev_spare_cap < 0`. This now correctly checks whether `prev_cpu` was never evaluated (still -1) rather than whether it had zero spare capacity.

4. **prev_cpu energy evaluation condition**: `prev_spare_cap > 0` becomes `prev_spare_cap > -1`. This now correctly evaluates `prev_cpu`'s energy when its spare capacity is 0 (since `0 > -1` is true), while still skipping when `prev_cpu` was not part of this performance domain (since `-1 > -1` is false).

The fix is correct and complete because -1 can never be a valid spare capacity (capacities are non-negative), and all four comparison points are updated consistently. The `max_spare_cap > prev_spare_cap` comparison on the later line already works correctly with signed values since both are now `long`.

## Triggering Conditions

To trigger this bug, the following precise conditions must be met:

1. **Asymmetric CPU capacity topology**: The system must have CPUs with different capacities (e.g., big.LITTLE ARM SoCs). EAS is only enabled on systems with `SD_ASYM_CPUCAPACITY` sched domains.

2. **EAS must be active**: This requires: (a) `CONFIG_ENERGY_MODEL` and `CONFIG_CPU_FREQ_GOV_SCHEDUTIL` enabled, (b) an energy model registered for all CPUs, (c) `schedutil` cpufreq governor active, (d) `arch_scale_freq_invariant()` returning true, (e) no SMT topology, and (f) `sysctl_sched_energy_aware` set to 1 (default).

3. **uclamp must be enabled**: `CONFIG_UCLAMP_TASK` must be set. The task must have its `uclamp_max` value set to a level that allows `util_fits_cpu()` to return true for a CPU even when the task's actual utilization exceeds the CPU's spare capacity.

4. **Task utilization must equal or exceed CPU capacity**: The task's `cpu_util()` value for a given CPU must be equal to or greater than `capacity_of(cpu)` so that `lsub_positive(&cpu_cap, util)` results in `cpu_cap = 0`. This is the key condition: a high-utilization task capped by `uclamp_max` to fit on a smaller CPU.

5. **The CPU must be the first (or only) viable candidate**: Since `max_spare_cap` starts at 0 (buggy) / -1 (fixed), the bug manifests on the very first CPU evaluated in a performance domain that has 0 spare capacity. If another CPU in the same domain has non-zero spare capacity, that CPU will be selected (since `cpu_cap > 0` is true) and the bug is partially masked — but the 0-spare-cap CPU is still unfairly excluded from consideration.

The bug is deterministic and 100% reproducible given the above conditions — it is a pure logic error with no race condition involved.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP due to fundamental dependencies on cpufreq and energy model infrastructure that QEMU does not provide.

### Why This Bug Cannot Be Reproduced With kSTEP

The bug exists in `find_energy_efficient_cpu()`, which is the Energy Aware Scheduling (EAS) wakeup path. EAS is gated by multiple checks in `sched_is_eas_possible()` (in `kernel/sched/topology.c`) that require hardware capabilities absent from QEMU:

1. **cpufreq driver requirement**: `sched_is_eas_possible()` calls `cpufreq_ready_for_eas(cpu_mask)`, which checks that all CPUs have a registered cpufreq policy with the `schedutil` governor active. QEMU does not emulate CPU frequency scaling hardware, so no cpufreq driver is loaded and `cpufreq_cpu_get()` returns NULL for all CPUs.

2. **Frequency-invariant load tracking**: `sched_is_eas_possible()` calls `arch_scale_freq_invariant()`, which returns true only when the architecture has a mechanism to report actual CPU frequencies for load tracking normalization. Without a cpufreq driver, this returns false.

3. **Energy model registration**: `build_perf_domains()` calls `pd_init(i)` which internally uses `em_cpu_get(i)` to retrieve the energy model for each CPU. Without a registered energy model (which comes from cpufreq drivers or devicetree-based EM registration), this returns NULL and performance domains cannot be built.

4. **Performance domain construction**: Even if the above checks could be bypassed, `build_perf_domains()` constructs `struct perf_domain` linked lists attached to the root domain's `rd->pd`. Without `rd->pd` being non-NULL, `find_energy_efficient_cpu()` returns immediately at the first check (`if (!pd || READ_ONCE(rd->overutilized)) goto unlock;`).

### What Would Need to Be Added to kSTEP

To support EAS-related bug reproduction, kSTEP would need these additions:

1. **Fake cpufreq driver**: A synthetic cpufreq driver that registers policies for all CPUs, supporting the `schedutil` governor. This driver would need to implement the `cpufreq_driver` interface including `init`, `verify`, `setpolicy`/`target`, and frequency table. This is a significant infrastructure addition.

2. **Fake energy model**: A helper like `kstep_em_register(cpumask, states[])` that calls `em_dev_register_perf_domain()` with synthetic performance states (frequency, power tuples) for groups of CPUs. This requires valid `struct device` pointers from `get_cpu_device()`.

3. **Schedutil governor activation**: The governor must be switched to `schedutil` after the fake cpufreq driver is registered. This could be a helper `kstep_cpufreq_set_governor("schedutil")`.

4. **Frequency-invariant load tracking**: The architecture's `arch_scale_freq_invariant()` must return true. On x86 (QEMU), this depends on hardware features (CPPC, HWP, or TSC) that may not be available in the emulated environment.

5. **uclamp task attribute setter**: A helper like `kstep_task_set_uclamp(p, UCLAMP_MAX, value)` that calls `sched_setattr_nocheck()` or directly modifies the task's uclamp values.

These changes collectively represent a major framework extension — essentially building a synthetic EAS environment — rather than minor API additions.

### Alternative Reproduction Methods

Outside kSTEP, this bug can be reproduced on real ARM big.LITTLE hardware (e.g., a Pixel phone, Raspberry Pi with heterogeneous cores, or an ARM development board) by:

1. Booting a kernel between v5.6 and v6.6 with `CONFIG_ENERGY_MODEL`, `CONFIG_CPU_FREQ_GOV_SCHEDUTIL`, and `CONFIG_UCLAMP_TASK` enabled.
2. Setting the cpufreq governor to `schedutil`.
3. Creating a CPU-intensive workload task and capping it with `uclamp_max` set to a low value (e.g., via `sched_setattr()` or cgroup `cpu.uclamp.max`).
4. Using the `sched_compute_energy_tp` tracepoint (added in patch 3/3 of the same series) or ftrace to observe whether `compute_energy()` is called for the LITTLE core performance domain.
5. On the buggy kernel, `compute_energy()` will not be called for the LITTLE cluster when spare capacity is 0; on the fixed kernel, it will be.
