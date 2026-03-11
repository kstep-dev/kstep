# psi: Fix cpu.pressure for cpu.max and competing cgroups

- **Commit:** b05e75d611380881e73edc58a20fd8c6bb71720b
- **Affected file(s):** kernel/sched/core.c, kernel/sched/psi.c, kernel/sched/stats.h
- **Subsystem:** PSI (Pressure Stall Information), core scheduler

## Bug Description

CPU pressure reporting was inaccurate for cgroups using cpu.max limits and in the presence of competing cgroups. The pressure metric was defined simply as "more than one runnable task," which failed to account for tasks being throttled by cpu.max limits or time during which competing cgroups occupied the CPU. This meant users could see zero or low CPU pressure even when their workload was being significantly constrained by cpu.max settings or external competition.

## Root Cause

The cpu pressure check only examined the number of runnable tasks (`NR_RUNNING > 1`) without accounting for how many tasks were actually executing on the CPU. With cpu.max throttling or competing cgroups, there would be runnable tasks waiting despite no actual CPU time being available, but the pressure metric would not reflect this constraint.

## Fix Summary

The fix introduces tracking of currently executing tasks via a new `NR_ONCPU` counter and redefines cpu pressure as `NR_RUNNING > ON_CPU` rather than simply `NR_RUNNING > 1`. A new `psi_sched_switch()` function updates this counter during context switches, and the `psi_dequeue()` function now clears the on-CPU state when tasks sleep, enabling accurate pressure reporting under cpu.max limits and competing cgroup scenarios.

## Triggering Conditions

This bug manifests in cgroups with CPU constraints, specifically:
- Cgroups configured with `cpu.max` throttling limits (e.g., 5000 10000 for 50% CPU)
- Tasks within throttled cgroups that would consume more CPU than the limit allows
- Multiple runnable tasks where some are blocked by throttling rather than actual CPU competition
- Alternatively, competing cgroups that occupy CPU time while other cgroups have runnable tasks waiting
- PSI must be enabled and monitoring CPU pressure within the affected cgroup
- The bug shows as artificially low or zero CPU pressure despite tasks being constrained by limits

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). In `setup()`: create cgroups with `kstep_cgroup_create()` and set CPU limits via `kstep_cgroup_write("testcg", "cpu.max", "5000 10000")` for 50% throttling. Create multiple tasks with `kstep_task_create()` and assign them to the throttled cgroup using `kstep_cgroup_add_task()`. In `run()`: wake tasks with `kstep_task_wakeup()` to make them runnable and compete for limited CPU. Use `kstep_tick_repeat()` to let throttling take effect. Monitor PSI pressure via `/sys/fs/cgroup/testcg/cpu.pressure` using `kstep_write()` to read values. Before the fix, pressure should show low values despite multiple runnable tasks being throttled. After the fix, pressure should correctly reflect the throttling constraint (~50% in this example). Use `on_tick_begin()` callback to log pressure readings and task states for comparison.
