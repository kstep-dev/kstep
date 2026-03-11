# cpufreq: schedutil: Update next_freq when cpufreq_limits change

- **Commit:** 9e0bc36ab07c550d791bf17feeb479f1dfc42d89
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** cpufreq schedutil

## Bug Description

When a CPU with constant maximum utilization has its `scaling_max_freq` reduced via sysfs, the `sg_policy->next_freq` is not updated to reflect the new frequency limit. The frequency remains stuck at the old maximum value. This prevents dynamic frequency limiting on busy CPUs, breaking an important operational capability where administrators need to cap CPU frequency while workloads are running.

## Root Cause

The `sugov_update_single_freq` function contains an optimization that prevents frequency reduction when the CPU is busy, assuming the reduction would be premature. However, this optimization does not account for the case where the policy's frequency limits themselves have changed (e.g., `scaling_max_freq` is lowered). When the `need_freq_update` flag is set to indicate that limits have changed, the optimization incorrectly suppresses the necessary frequency update.

## Fix Summary

The fix adds a check for the `need_freq_update` flag to the condition that prevents frequency reduction on busy CPUs. When policy limits change (`need_freq_update` is set), the frequency is updated regardless of CPU busyness, allowing the new limit to take effect. For normal cases where limits haven't changed, the original optimization is preserved.

## Triggering Conditions

The bug requires a CPU running under the schedutil cpufreq governor with constant maximum utilization that saturates CPU capacity. The CPU must be continuously busy (e.g., via a CPU-bound task pinned to it) so that `sugov_cpu_is_busy()` returns true. When the policy's `scaling_max_freq` is reduced via sysfs while the CPU remains busy, the `sg_policy->next_freq` should update to the new lower limit, but the optimization in `sugov_update_single_freq()` prevents this. The `need_freq_update` flag gets set when policy limits change, but the busy CPU condition takes precedence and keeps the old higher frequency, violating the new constraint.

## Reproduce Strategy (kSTEP)

Use 2 CPUs minimum (CPU 0 reserved for driver). Create a CPU-bound task pinned to CPU 1 using `kstep_task_create()` and `kstep_task_pin(task, 1, 1)`. Run the task for several ticks with `kstep_tick_repeat()` to ensure CPU 1 reaches maximum utilization. Check that the frequency reaches maximum via cpufreq policy inspection. Then simulate scaling_max_freq reduction using `kstep_write()` to modify the appropriate sysfs file (e.g., `/sys/devices/system/cpu/cpufreq/policy1/scaling_max_freq`). Continue ticking and monitor the actual frequency through policy state to verify if it gets stuck at the old maximum instead of updating to the new limit. Use `on_tick_begin` callback to log frequency values and confirm the bug manifestation.
