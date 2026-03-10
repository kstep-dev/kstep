# sched/pelt: Fix attach_entity_load_avg() corner case

- **Commit:** 40f5aa4c5eaebfeaca4566217cb9c468e28ed682
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** PELT (Per-Entity Load Tracking)

## Bug Description

A corner case in `attach_entity_load_avg()` causes `load_sum` to become zero while `load_avg` remains non-zero (specifically = 1). This violates the invariant that `load_avg` should only be non-zero when `load_sum` is also non-zero, triggering a kernel warning in `cfs_rq_is_decayed()`. The bug occurs when the integer division `(load_avg * divider) / se_weight` truncates to zero due to the specific ratio of values.

## Root Cause

The original code initializes `se->avg.load_sum = divider`, then overwrites it with `div_u64(se->avg.load_avg * divider / se_weight)`. With certain weight and divider values (e.g., se_weight=88761, divider=47742), the division results in zero while load_avg=1, creating an inconsistent state where the cfs_rq has non-zero load_avg but zero load_sum.

## Fix Summary

The fix ensures that `load_sum` is computed directly from `load_avg * divider`, and guards against truncation to zero by explicitly setting `load_sum = 1` when division would produce zero. This maintains the invariant that non-zero load_avg always has a corresponding non-zero load_sum.

## Triggering Conditions

The bug occurs in `attach_entity_load_avg()` when a task with non-zero `load_avg` gets attached to a cfs_rq. The specific conditions are:
- A task's sched_entity has `se->avg.load_avg = 1` (minimum non-zero value)
- The task's weight (`se_weight(se)`) must be significantly larger than the PELT divider value
- Example: `se_weight = 88761` (nice -19 from sched_prio_to_weight table), `divider = 47742`
- Division `(load_avg * divider) / se_weight = (1 * 47742) / 88761 = 0` truncates to zero
- This creates inconsistent PELT state where `load_avg = 1` but `load_sum = 0`
- The warning triggers later in `cfs_rq_is_decayed()` when checking PELT invariants

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. Create a task with maximum nice value (-19) to get large weight (88761):
- In `setup()`: Create task with `kstep_task_create()` and set to nice -19 with `kstep_task_set_prio(task, -19)`
- Let task run briefly to accumulate minimal `load_avg = 1`, then pause with `kstep_task_pause()`
- Wait for PELT decay to reach specific divider values around 47742 via `kstep_tick_repeat()`
- Wake task with `kstep_task_wakeup()` to trigger `attach_entity_load_avg()` path
- Use `on_tick_end()` callback to monitor PELT values and detect inconsistent state
- Log `se->avg.load_avg` and `se->avg.load_sum` after attachment
- Detect bug when `load_avg > 0` but `load_sum = 0`, which violates PELT invariants
