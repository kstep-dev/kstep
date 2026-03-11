# cpufreq/sched: Explicitly synchronize limits_changed flag handling

- **Commit:** 79443a7e9da3c9f68290a8653837e23aba0fa89f
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** cpufreq

## Bug Description

Without explicit synchronization of the `limits_changed` flag, CPU policy limits updates can be missed due to memory reordering by the CPU or compiler. Specifically, when the `limits_changed` flag update in `sugov_limits()` is reordered relative to policy limit changes, or when the update in `sugov_should_update_freq()` is reordered relative to policy limit reads, the new CPU frequency policy limits may not take effect for a prolonged period.

## Root Cause

The race condition arises from a lack of memory ordering guarantees around the `limits_changed` flag. Without explicit memory barriers and compiler hints (READ_ONCE/WRITE_ONCE), the CPU and compiler can reorder the flag update relative to the actual policy limit value changes. This creates a window where `sugov_should_update_freq()` reads stale policy limits even though `limits_changed` has been set to false, causing the new limits to be ignored.

## Fix Summary

The fix adds memory barriers (`smp_mb()` in `sugov_should_update_freq()` and `smp_wmb()` in `sugov_limits()`) and wraps all `limits_changed` flag accesses with `READ_ONCE()` and `WRITE_ONCE()` to prevent compiler reordering and ensure proper synchronization between policy limit updates and the flag handling code.

## Triggering Conditions

This memory ordering bug requires precise race timing between two concurrent paths:
- **Path 1**: `sugov_limits()` called during cpufreq policy limit changes (e.g., via sysfs writes to scaling_max_freq)
- **Path 2**: `sugov_should_update_freq()` called during scheduler frequency updates from `cpufreq_update_util()`
- **Critical timing**: `sugov_should_update_freq()` must execute between the policy limit updates in `cpufreq_set_policy()` and the `limits_changed = true` assignment in `sugov_limits()`
- **Memory reordering**: Without barriers, the flag update can be reordered past policy limit reads, causing stale limits to be used
- **Sustained high CPU activity**: Needed to trigger frequent scheduler updates and increase race window probability

## Reproduce Strategy (kSTEP)

Reproduce using multi-CPU concurrent frequency scaling and scheduler activity:
- **Setup**: 2+ CPUs (driver on CPU 0, target CPUs 1+), enable schedutil governor, configure frequency scaling sysfs paths
- **Tasks**: Create high-frequency scheduling activity with `kstep_task_create()` + `kstep_task_wakeup()` on target CPUs
- **Race trigger**: Use `kstep_sysctl_write()` to modify `/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq` concurrently with scheduler ticks
- **Timing**: Alternate between `kstep_tick()` calls and cpufreq limit changes in tight loops to maximize race probability
- **Detection**: Use `on_tick_begin()` callback to log policy limits and `limits_changed` flag state; detect when new limits don't take effect
- **Validation**: Compare observed frequency behavior against expected policy limits; stale limits indicate the bug was triggered
