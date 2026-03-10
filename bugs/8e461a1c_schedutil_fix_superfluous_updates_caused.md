# cpufreq: schedutil: Fix superfluous updates caused by need_freq_update

- **Commit:** 8e461a1cb43d69d2fc8a97e61916dce571e6bb31
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** EXT (cpufreq scheduling utility)

## Bug Description

The cpufreq scheduler utility performs superfluous frequency updates in two scenarios. First, drivers specifying `CPUFREQ_NEED_UPDATE_LIMITS` receive frequency updates all the time, not just when policy limits actually change, because `need_freq_update` is never cleared. Second, the `ignore_dl_rate_limit()` usage of `need_freq_update` causes redundant frequency updates when the next chosen frequency is the same as the current one, regardless of driver flags. This leads to unnecessary CPU power consumption and frequency scaling operations.

## Root Cause

The `need_freq_update` flag was set unconditionally to `true` on policy limits changes (line 75), and then checked in `sugov_update_next_freq()` where it was set to `CPUFREQ_NEED_UPDATE_LIMITS` (line 99). This logic caused the flag to persist and trigger updates on subsequent calls even when not needed. The flag was never cleared after the redundant update occurred, leading to continuous unnecessary updates.

## Fix Summary

The fix moves the `CPUFREQ_NEED_UPDATE_LIMITS` check from `sugov_update_next_freq()` to `sugov_should_update_freq()` where limits changes are detected, setting `need_freq_update` appropriately only for drivers that need it. Then, `need_freq_update` is cleared to `false` in `sugov_update_next_freq()` after being used, ensuring the flag doesn't persist across multiple calls and cause superfluous updates.

## Triggering Conditions

The bug manifests in the cpufreq schedutil governor subsystem when `sugov_should_update_freq()` and `sugov_update_next_freq()` handle frequency updates. Two scenarios trigger superfluous updates: (1) drivers specifying `CPUFREQ_NEED_UPDATE_LIMITS` experience continuous frequency updates because `need_freq_update` remains set to `true` after policy limits changes and is never cleared, and (2) deadline tasks using `ignore_dl_rate_limit()` trigger redundant updates when the computed next frequency equals the current frequency. The bug requires workloads that change CPU utilization patterns, policy limit modifications, or deadline task activation that bypasses rate limiting. The persistent `need_freq_update` flag causes subsequent scheduler invocations to unnecessarily call the cpufreq driver.

## Reproduce Strategy (kSTEP)

Reproduce by creating CPU utilization changes that trigger cpufreq updates while monitoring frequency update calls. Setup requires 2+ CPUs and cpufreq schedutil governor enabled. In `setup()`, use `kstep_sysctl_write("kernel.sched_util_clamp_min", "0")` to ensure utilization clamping works. Create mixed workload with `kstep_task_create()` for CFS tasks and deadline tasks. In `run()`, simulate policy limits changes by modifying CPU frequency scaling via sysfs writes with `kstep_write()` to trigger `limits_changed=true`. Create deadline tasks with `kstep_task_create()` and modify priority. Use `kstep_tick_repeat()` to advance time and trigger multiple frequency evaluations. Monitor via custom logging in callbacks to track `need_freq_update` persistence across multiple `sugov_should_update_freq()` calls. The bug manifests as repeated frequency updates when utilization doesn't require them, detectable by excessive cpufreq driver invocations.
