# sched/uclamp: Fix iowait boost escaping uclamp restriction

- **Commit:** d37aee9018e68b0d356195caefbb651910e0bbfa
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** cpufreq, uclamp

## Bug Description

The iowait_boost signal bypasses uclamp_max restrictions on CPU frequency requests. When an I/O-heavy task is capped by uclamp_max to limit its frequency, the iowait boost mechanism can still request higher frequencies by applying boost independently without checking the uclamp constraints. This defeats the purpose of uclamp frequency capping, allowing tasks to exceed their intended frequency limits during I/O waits.

## Root Cause

The function `sugov_iowait_apply()` calculates the iowait boost value and applies it to the CPU utilization without clamping it through uclamp restrictions. In contrast, `effective_cpu_util()` properly clamps utilization via `uclamp_rq_util_with()`. This inconsistency creates a path where iowait_boost can escape the uclamp frequency cap that should apply to all requests.

## Fix Summary

The fix adds a single call to `uclamp_rq_util_with(cpu_rq(sg_cpu->cpu), boost, NULL)` immediately after calculating the boost value in `sugov_iowait_apply()`. This ensures the iowait_boost is clamped according to the rq's uclamp settings, making it honor the same frequency restrictions as regular utilization-based requests.

## Triggering Conditions

The bug requires an I/O-heavy task with uclamp_max restrictions under the schedutil cpufreq governor. The task must:
- Be configured with uclamp_max lower than maximum frequency capability
- Perform frequent I/O operations that trigger iowait_boost accumulation  
- Experience blocked I/O waits that activate `sugov_iowait_apply()`
- Have the boost value calculated exceed the uclamp_max limit

The issue manifests when `sugov_iowait_apply()` applies the unclamped boost directly to `sg_cpu->util`, bypassing uclamp restrictions. The timing requires the iowait_boost to accumulate to values that would be reduced by uclamp_max clamping, creating observable frequency requests above the intended limit.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved). In `setup()`, create a cgroup with uclamp_max restriction using `kstep_cgroup_create()` and `kstep_cgroup_write()` to set uclamp.max. Create an I/O-heavy task with `kstep_task_create()` and add to cgroup with `kstep_cgroup_add_task()`.

In `run()`, simulate I/O waits by repeatedly calling `kstep_task_pause()` and `kstep_task_wakeup()` to trigger iowait boost accumulation. Use `kstep_tick_repeat()` between operations to allow boost to build up. Monitor through `on_tick_begin()` callback to log CPU utilization before/after iowait_apply.

Detection: Compare actual CPU utilization requests against expected uclamp_max limits. Log boost values and final util in sugov_iowait_apply to verify if boost exceeds uclamp restrictions. The bug occurs when boost values bypass clamping and result in higher frequency requests than uclamp_max allows.
