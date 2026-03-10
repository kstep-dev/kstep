# cpufreq: schedutil: restore cached freq when next_f is not changed

- **Commit:** 0070ea29623904224c0f5fa279a16a4ac9223295
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** cpufreq

## Bug Description

The raw cached frequency value is reset to 0 when the governor avoids frequency reduction due to the CPU not being idle recently. This causes unnecessary recalculation and cpufreq driver calls in subsequent cycles, even when the frequency should remain unchanged, defeating the purpose of the cache optimization on cost-sensitive architectures.

## Root Cause

The code unconditionally resets `sg_policy->cached_raw_freq` to 0 when avoiding frequency reduction, discarding the previous cached value. This forces the next update cycle to recalculate the frequency even if the optimal frequency hasn't changed, leading to potentially costly cpufreq driver invocations.

## Fix Summary

The fix saves the cached frequency value at the start of the function and restores it (instead of resetting to 0) when avoiding frequency reduction. This preserves the cache across update cycles where the frequency decision doesn't change, reducing unnecessary cpufreq driver calls.

## Triggering Conditions

The bug occurs in the schedutil cpufreq governor's `sugov_update_single()` function when:
- A CPU has the schedutil cpufreq governor active
- The CPU transitions from idle to busy state (sugov_cpu_is_busy() returns true)
- A frequency update is triggered that would normally reduce frequency
- The frequency reduction is avoided due to the CPU not being idle recently
- The `sg_policy->cached_raw_freq` has a non-zero value from previous cycles
- On the avoided frequency reduction, cached_raw_freq gets reset to 0 instead of preserved
- Subsequent frequency update cycles must recalculate even when the optimal frequency is unchanged

## Reproduce Strategy (kSTEP)

This bug is in the cpufreq subsystem rather than core scheduler, making direct reproduction challenging with kSTEP's current scheduler-focused API:
- Setup: Single CPU system with varying workload (1 CPU beyond the reserved CPU 0)
- Create 2-3 tasks with different nice values to generate utilization fluctuations
- Use `kstep_task_wakeup()` and `kstep_task_pause()` to create busy/idle transitions
- In `on_tick_begin()`, log CPU utilization and trigger frequency updates if possible
- Use `kstep_tick_repeat()` with varying intervals to simulate time-based frequency decisions
- Monitor for unnecessary cpufreq driver calls when frequency should remain cached
- Detection requires observing cpufreq governor state, which may need custom kernel instrumentation
- Alternative: Focus on the conditions that trigger the code path rather than the caching bug itself
