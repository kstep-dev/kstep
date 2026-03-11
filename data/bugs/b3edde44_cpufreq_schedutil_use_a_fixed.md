# cpufreq/schedutil: Use a fixed reference frequency

- **Commit:** b3edde44e5d4504c23a176819865cd603fd16d6c
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** Core scheduler (DVFS/cpufreq integration)

## Bug Description

When CPU boost or other frequency scaling features cause `cpuinfo.max_freq` to change at runtime, the reference frequency used for computing the scaled utilization becomes inconsistent with the reference frequency originally used to compute the CPU's capacity. This causes the frequency scaling decisions to be incorrect, as the mapping between utilization and frequency is no longer aligned with how capacity was originally calculated.

## Root Cause

The old code in `get_next_freq()` used `policy->cpuinfo.max_freq` (which can change at runtime due to boost) or `policy->cur` as the reference frequency for frequency calculations. However, CPU capacity is computed based on a fixed reference frequency that was established at system initialization. When `cpuinfo.max_freq` changes after capacity has been computed, the two reference frequencies diverge, leading to incorrect frequency selection.

## Fix Summary

The fix introduces a new `get_capacity_ref_freq()` helper function that retrieves a fixed reference frequency via `arch_scale_freq_ref()`, which is guaranteed not to change at runtime. If that function is not available, it falls back to the original logic. This ensures that the reference frequency used for scaling utilization to frequency remains consistent with the reference frequency used when computing CPU capacity.

## Triggering Conditions

This bug is triggered when CPU boost or other dynamic frequency scaling features cause `policy->cpuinfo.max_freq` to change at runtime after CPU capacities have been computed at system initialization. The schedutil governor's `get_next_freq()` function uses the current `max_freq` or `cur` frequency as reference for utilization-to-frequency mapping, creating a mismatch with the original fixed reference frequency used for capacity calculation. This occurs specifically in the cpufreq_schedutil governor when `arch_scale_freq_invariant()` is true and frequency scaling is active. The bug manifests when utilization calculations produce incorrect frequency targets because the reference frequency baseline has shifted, leading to suboptimal DVFS decisions and potential performance or power inefficiency.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (driver uses CPU 0). Set different CPU frequencies using `kstep_cpu_set_freq()` to simulate varying `cpuinfo.max_freq` values. Create tasks with `kstep_task_create()` and pin them using `kstep_task_pin()` to specific CPUs. In `setup()`, establish baseline CPU capacities and frequencies. In `run()`, use `kstep_tick_repeat()` to generate utilization that triggers schedutil frequency scaling. Use `kstep_sysctl_write()` to modify cpufreq governor settings if needed. Monitor frequency scaling decisions via custom logging in `on_tick_begin()` callback. To detect the bug, compare expected vs actual frequency selections by accessing schedutil policy structures through kernel interfaces, logging when utilization-to-frequency mapping produces inconsistent results due to reference frequency mismatches. The reproduction depends on having dynamic frequency scaling active and detectable changes in frequency reference points.
