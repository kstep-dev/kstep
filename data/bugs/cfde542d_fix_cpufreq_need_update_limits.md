# cpufreq/sched: Fix the usage of CPUFREQ_NEED_UPDATE_LIMITS

- **Commit:** cfde542df7dd51d26cf667f4af497878ddffd85a
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** cpufreq/sched

## Bug Description

When CPU frequency policy limits change, the scheduler's cpufreq governor must invoke the driver callback so the driver can apply the new limits. However, a previous optimization incorrectly skipped the driver callback for drivers without the CPUFREQ_NEED_UPDATE_LIMITS flag, even when the policy limits had changed. This causes some drivers to silently miss policy limit updates, resulting in incorrect frequency scaling behavior and potential failure to enforce new power/thermal constraints.

## Root Cause

Commit 8e461a1cb43d attempted to reduce superfluous frequency updates by setting the need_freq_update flag only for drivers with CPUFREQ_NEED_UPDATE_LIMITS set. This optimization ignored the fact that the driver callback must be invoked when policy limits change, regardless of the flag state. The code only checked whether the resulting frequency was the same, not whether the constraints themselves had changed, causing drivers to miss limit changes entirely.

## Fix Summary

The fix restores the unconditional setting of need_freq_update when limits change, then applies a more nuanced optimization: the driver callback is skipped only if both the resulting frequency is unchanged AND the driver doesn't require explicit notification of limit changes (CPUFREQ_NEED_UPDATE_LIMITS not set). This ensures all drivers receive necessary limit updates while still avoiding redundant callbacks when the frequency wouldn't change anyway.

## Triggering Conditions

This bug occurs in the schedutil cpufreq governor when policy limits change. The specific conditions are:
- A cpufreq driver that does NOT have the CPUFREQ_NEED_UPDATE_LIMITS flag set
- CPU frequency policy limits are modified (triggers sg_policy->limits_changed = true)
- The sugov_should_update_freq() function is called during frequency evaluation
- In the buggy kernel, need_freq_update remains false, causing the driver callback to be skipped
- The driver silently misses the limit update, potentially violating new thermal/power constraints

## Reproduce Strategy (kSTEP)

Reproducing this bug requires triggering cpufreq policy limit changes and monitoring driver callback behavior:
- Setup: Use at least 2 CPUs; create tasks to generate scheduler activity that triggers frequency scaling
- Use kstep_sysctl_write() to modify cpufreq-related sysctls (e.g., scaling_max_freq, scaling_min_freq)
- Create CPU load patterns with kstep_task_create() and kstep_tick_repeat() to force frequency updates
- Monitor sugov_should_update_freq() calls using kernel tracing or custom logging
- Use on_tick_begin() callback to capture frequency scaling events and policy state
- Compare driver callback invocation between buggy and fixed kernels
- Detection: Log when limits_changed=true but need_freq_update=false in buggy version
- Expected: Driver callbacks are missed in buggy kernel but properly invoked after the fix
