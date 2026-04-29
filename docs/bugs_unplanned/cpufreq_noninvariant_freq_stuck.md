# Cpufreq: Frequency selection stuck at current OPP on non-invariant systems

**Commit:** `e37617c8e53a1f7fcba6d5e1041f4fd8a2425c27`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v6.8-rc1
**Buggy since:** v6.8-rc1 (introduced by `9c0b4bb7f630` "sched/cpufreq: Rework schedutil governor performance estimation", merged in the same rc cycle)

## Bug Description

When CPU frequency invariance is not enabled (i.e., `arch_scale_freq_invariant()` returns false), the schedutil governor's frequency selection logic becomes unable to select a frequency higher than the current one, even when the CPU is fully utilized. This causes the CPU to remain stuck at the initial low frequency, resulting in a catastrophic ~50% performance regression on single-threaded and multi-threaded workloads.

The bug was reported by Linus Torvalds himself on his 32-core (64-thread) AMD Ryzen Threadripper 3970X system, where an empty kernel build went from 22 seconds to 44 seconds. Wyes Karny independently confirmed the issue on an AMD Ryzen 5600G system when CPPC was disabled in BIOS, forcing the system to use acpi-cpufreq with the schedutil governor. On systems where frequency invariance is not supported (common on older x86 systems without CPPC or without `arch_scale_freq_ref()` support), the CPU frequency was effectively locked at its boot-time value.

The regression was introduced by commit `9c0b4bb7f630` which reworked the schedutil governor's performance estimation to better handle uclamp hints. As part of that rework, the 25% performance margin (applied via `map_util_perf()`) was moved from `get_next_freq()` to the earlier `sugov_effective_cpu_perf()` function. This relocation was intentional for the frequency-invariant case, but it inadvertently broke the non-invariant case by removing the headroom that allowed utilization-based frequency computation to request a higher OPP.

## Root Cause

The core of the problem lies in the interaction between `get_capacity_ref_freq()` and the frequency computation in `get_next_freq()`. The frequency selection formula in `get_next_freq()` is:

```c
next_freq = ref_freq * util / max
```

where `ref_freq` is the value returned by `get_capacity_ref_freq()`. In the non-invariant case, `ref_freq` was set to `policy->cur` (the current CPU frequency).

Before commit `9c0b4bb7f630`, `get_next_freq()` applied a 25% performance margin via `map_util_perf(util)` (which computes `util + util >> 2`, effectively `util * 1.25`). This meant that even when `util == max` (CPU fully loaded), the computation would yield:

```
next_freq = policy->cur * (max * 1.25) / max = policy->cur * 1.25
```

This 25% headroom above the current frequency ensured that the governor would request a higher OPP when the CPU was approaching full utilization at its current frequency, creating a natural feedback loop that ramped frequency up to meet demand.

After commit `9c0b4bb7f630`, the `map_util_perf()` call was removed from `get_next_freq()` and moved into `sugov_effective_cpu_perf()`, which applies the performance margin earlier in the pipeline. However, `sugov_effective_cpu_perf()` explicitly caps the boosted utilization: `if (actual < max) max = actual;` and then returns `max(min, max)`. This means the utilization value passed to `get_next_freq()` can never exceed `max` (the CPU capacity). Consequently, the formula becomes:

```
next_freq = policy->cur * max / max = policy->cur
```

The governor always computes the next frequency as exactly the current frequency, creating a self-referential loop where the CPU can never ramp up. This is a classic bootstrapping problem: the reference frequency equals the current frequency, and utilization is capped at 100% of capacity, so the computed target frequency is always the current one.

Wyes Karny's bpftrace data confirmed this: in the buggy case, `sugov_max_cap` was always 0 (because on non-invariant AMD systems without CPPC, `arch_scale_cpu_capacity()` returns 0), and `sugov_freq` was locked at 1.4 GHz (the lowest OPP). In the working case, the frequency distribution was spread across the full range up to 3.9 GHz.

## Consequence

The consequence is a severe, deterministic performance degradation on any system where CPU frequency invariance is not enabled. This includes:

- Older x86 systems without CPPC or hardware P-state feedback
- AMD systems with CPPC disabled in BIOS, using acpi-cpufreq
- Any platform relying on `acpi-cpufreq` + `schedutil` without `arch_scale_freq_ref()` support

The CPU remains stuck at its initial (typically lowest) frequency regardless of workload, causing approximately 50% performance loss for compute-bound workloads as measured by Linus Torvalds. The regression is immediately noticeable in any CPU-bound benchmark or compilation workload. There is no crash or kernel warning — the system simply runs at a fraction of its potential performance.

The impact was severe enough that Ingo Molnar initially proposed reverting the entire set of schedutil rework commits (`9c0b4bb7f630`, `f12560779f9d`, `b3edde44e5d4`) from the v6.8 merge window. The fix was subsequently developed and applied instead, allowing the schedutil rework to remain.

## Fix Summary

The fix modifies the `get_capacity_ref_freq()` function to return `policy->cur + (policy->cur >> 2)` instead of `policy->cur` in the non-invariant case. This adds a 25% margin to the reference frequency, effectively restoring the headroom that was previously provided by `map_util_perf()` inside `get_next_freq()`.

With this fix, when the CPU is fully utilized (`util == max`), the frequency computation becomes:

```
next_freq = (policy->cur * 1.25) * max / max = policy->cur * 1.25
```

This ensures the governor requests a frequency 25% higher than the current one when the CPU is at full load, which causes the cpufreq framework to select the next available OPP above the current frequency. This restores the natural frequency ramp-up behavior: as the CPU approaches full utilization at one OPP, the governor requests a higher one, until the workload demand is met.

The fix is minimal (a single-line change with a comment) and precisely targets the regression. It was reviewed and tested by Qais Yousef, Dietmar Eggemann (on an Intel Xeon CPU E5-2690 v2 with frequency invariance disabled), and Wyes Karny (on AMD Ryzen 5600G with CPPC disabled). The approach is correct because it re-establishes the C=1.25 tipping point behavior that the schedutil governor was always designed to have, just applying it at the reference frequency level rather than at the utilization level.

## Triggering Conditions

The bug triggers under the following precise conditions:

1. **Frequency invariance disabled**: `arch_scale_freq_invariant()` must return false. This is the case on x86 systems using `acpi-cpufreq` without `CONFIG_X86_AMD_CPPC` or with CPPC disabled in BIOS, and on any architecture that does not provide frequency invariance counters.

2. **No `arch_scale_freq_ref()` override**: The architecture must not provide a non-zero return from `arch_scale_freq_ref()`. If this function returns a valid reference frequency, the bug does not trigger because that value is used instead of `policy->cur`.

3. **Schedutil governor active**: The system must be using the `schedutil` cpufreq governor (not `ondemand`, `performance`, or `powersave`).

4. **Any CPU-bound workload**: Any workload that drives CPU utilization near 100% will exhibit the bug. The CPU will remain at its lowest or initial OPP instead of ramping up. Simple benchmarks like `make -j1` or single-threaded compilations clearly demonstrate the regression.

5. **System booted with low initial frequency**: The regression is most apparent when the CPU starts at a low OPP. Since the governor cannot ramp up, the initial frequency determines maximum performance.

The bug is fully deterministic — it occurs on every schedutil frequency update on affected systems. There is no race condition or timing dependency; the logic error unconditionally prevents frequency ramp-up.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **QEMU has no cpufreq hardware or driver**: The bug is entirely within the schedutil cpufreq governor's frequency selection logic (`get_capacity_ref_freq()` → `get_next_freq()` → `cpufreq_driver_resolve_freq()`). Reproducing it requires a registered cpufreq driver that provides a `policy->cur` value and a set of available OPPs (operating performance points). QEMU does not emulate cpufreq hardware, so there is no cpufreq driver loaded, no cpufreq policy object, and the schedutil governor is never instantiated or invoked.

2. **No frequency invariance infrastructure in QEMU**: The bug specifically targets the code path where `arch_scale_freq_invariant()` returns false and `arch_scale_freq_ref()` returns 0. While QEMU technically satisfies both conditions (no frequency scaling hardware), the entire cpufreq subsystem is absent, so the buggy code path is never reached.

3. **kSTEP's `kstep_cpu_set_freq()` only changes the frequency scale factor**: kSTEP provides `kstep_cpu_set_freq(cpu, scale)` which writes to `per_cpu(arch_freq_scale, cpu)`, affecting the PELT frequency scaling factor. This does not create a cpufreq policy, register a cpufreq driver, or instantiate the schedutil governor. The entire cpufreq governor callchain (`sugov_get_util()` → `sugov_effective_cpu_perf()` → `get_next_freq()` → `get_capacity_ref_freq()`) is never invoked.

4. **Fundamental architectural gap**: To reproduce this bug, kSTEP would need to either:
   - Implement a virtual cpufreq driver that registers with the cpufreq framework, creates policy objects, provides OPP tables, and integrates with the schedutil governor callback mechanism.
   - Or provide a way to directly invoke the schedutil frequency computation path and observe the resulting frequency selection.
   
   The first option requires adding a complete virtual cpufreq driver subsystem to kSTEP (e.g., `kstep_cpufreq_register()`, `kstep_cpufreq_set_opps()`, `kstep_cpufreq_set_governor("schedutil")`). This is a fundamental addition, not a minor extension.

5. **Observable impact requires real frequency changes**: Even if the computation could be triggered, the actual performance degradation (the ~50% slowdown) manifests through the CPU running at a lower physical frequency. In QEMU, all virtual CPUs run at the same emulated speed regardless of any cpufreq state, so the performance impact cannot be observed.

6. **Alternative reproduction methods**: The bug can be reliably reproduced on any bare-metal x86 system without frequency invariance support (or with it disabled). Dietmar Eggemann confirmed reproduction on Intel Xeon CPU E5-2690 v2 by disabling frequency invariance. The simplest reproduction is:
   - Boot a non-invariant x86 system with `cpufreq.default_governor=schedutil`
   - Run `taskset -c 1 make` or any single-threaded CPU-bound workload
   - Observe via `cpupower monitor` or `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` that the frequency remains stuck at the lowest OPP
   - Compare wall-clock time against a kernel with the fix applied
