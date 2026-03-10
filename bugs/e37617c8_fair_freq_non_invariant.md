# sched/fair: Fix frequency selection for non-invariant case

- **Commit:** e37617c8e53a1f7fcba6d5e1041f4fd8a2425c27
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** cpufreq

## Bug Description

On systems where frequency invariance is not enabled, CPUs get stuck at lower frequencies instead of switching to higher OPPs, causing approximately 50% performance regression on single-threaded workloads. The scheduler is unable to select a higher frequency even when the current frequency becomes fully utilized because the reference frequency used for frequency selection is clamped to the current frequency.

## Root Cause

A previous refactoring (9c0b4bb7f630) moved the performance margin application earlier in the frequency selection path to account for utilization clampings. However, when frequency invariance is disabled, the function `get_capacity_ref_freq()` returns `policy->cur` as the reference frequency. Without a margin applied at this point, the utilization cannot exceed the maximum compute capacity at that frequency, preventing the selection of a higher OPP even when the current one becomes fully used.

## Fix Summary

The fix modifies `get_capacity_ref_freq()` to return a frequency 25% higher than the current frequency (`policy->cur + (policy->cur >> 2)`) in the non-invariant case. This provides headroom for the frequency selection logic to proactively switch to a higher OPP before the current frequency is fully saturated, restoring performance on systems without frequency invariance support.

## Triggering Conditions

This bug occurs specifically on systems where frequency invariance is disabled (arch_scale_freq_invariant() returns false). The cpufreq schedutil governor path through `get_next_freq()` calls `get_capacity_ref_freq()` which returns `policy->cur` without margin. When a single-threaded workload fully utilizes the CPU at the current frequency, the utilization cannot exceed maximum capacity at that frequency, preventing selection of higher OPPs. The bug manifests when the CPU becomes busy enough to warrant frequency scaling but the utilization calculation gets clamped by the current frequency reference, causing the CPU to remain "stuck" at lower frequencies instead of scaling up appropriately.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). In `setup()`, disable frequency invariance via sysctl or simulate non-invariant conditions. Create a single CPU-bound task using `kstep_task_create()` and pin it to CPU 1 with `kstep_task_pin(task, 1, 1)`. In `run()`, start with a low frequency using `kstep_cpu_set_freq(1, low_scale)`, wake the task with `kstep_task_wakeup(task)`, then run `kstep_tick_repeat(100)` to simulate sustained CPU load. Use `on_tick_begin()` callback to monitor CPU frequency and utilization via `/sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq`. The bug is triggered when utilization reaches 100% at current frequency but frequency selection fails to scale up to higher OPPs, observable through frequency logs remaining at initial low value despite high utilization.
