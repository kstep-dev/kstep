# sched: Fix priority inversion of cookied task with sibling

- **Commit:** 7afbba119f0da09824d723f8081608ea1f74ff57
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** Core scheduling (core-wide task selection)

## Bug Description

The core-wide task selection logic incorrectly skips checking other CPUs for higher-priority RT tasks when the current CPU has no cookie and `need_sync` is false. This causes priority inversion: a lower-priority tagged CFS task gets scheduled on the current CPU while a higher-priority untagged RT task on a sibling CPU remains idle. The bug occurs when a core contains multiple CPUs with mixed tagged and untagged tasks of different priority classes.

## Root Cause

The original code had two problematic conditions: (1) it skipped non-local CPU checks entirely for fair_sched_class when `need_sync == false`, and (2) it would immediately return a picked task without ensuring that all CPUs are checked for higher-priority RT tasks. The root cause is that `need_sync` was only set to true after selecting the first task (in the `old_max` case), not when starting the selection loop, leading to incomplete cross-CPU synchronization checks.

## Fix Summary

The fix reorganizes the logic to set `need_sync` at the beginning based on whether any CPU has a cookie, and adds an optimization path that only applies when the picked task has no cookie. It removes the early-exit logic that would skip RT entirely, ensuring all CPUs are evaluated when cookies are involved or when the selected task itself is tagged, maintaining proper priority ordering across the core.

## Triggering Conditions

Core scheduling must be enabled with SMT sibling CPUs where one CPU has tagged (cookied) CFS tasks and a sibling CPU has an untagged RT task. The bug triggers when `schedule()` is called on the CPU with tagged tasks while `need_sync` is initially false. The RT task on the sibling CPU must have higher priority than the CFS tasks. The core's `core_cookie` being initially zero causes the incomplete cross-CPU priority checks, leading to the RT task remaining idle while a lower-priority tagged CFS task gets scheduled instead.

## Reproduce Strategy (kSTEP)

Configure 2+ CPUs as SMT siblings using `kstep_topo_set_smt()` with CPU pairs like "1-2". Create tagged CFS tasks on CPU 1 using `kstep_task_create()`, `kstep_task_cfs()`, and tag them via `kstep_cgroup_create()`/`kstep_cgroup_add_task()`. Create an untagged RT task on CPU 2 using `kstep_task_fifo()`. Pin tasks to specific CPUs with `kstep_task_pin()`. Use `kstep_tick()` to trigger scheduling decisions on the CFS CPU. Implement `on_tick_begin()` callback to log current task per CPU and detect if the RT task remains idle while the lower-priority CFS task runs, indicating priority inversion.
