# sched/uclamp: Fix wrong implementation of cpu.uclamp.min

- **Commit:** 0c18f2ecfcc274a4bcc1d122f79ebd4001c3b445
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (uclamp)

## Bug Description

The cpu.uclamp.min cgroup setting was incorrectly implemented as a limit rather than a protection. This caused tasks attached to a cgroup with uclamp.min set to behave incorrectly: a task with a lower min value would have its performance capped to the cgroup's minimum, rather than being boosted to meet the cgroup's minimum guarantee. Additionally, tasks without explicitly set user-defined clamps would be incorrectly overridden by the cgroup's clamp values.

## Root Cause

The `uclamp_tg_restrict()` function treated both UCLAMP_MIN and UCLAMP_MAX identically, comparing against a single upper bound and applying the same logic (`if (uc_req.value > uc_max.value) return uc_max`). This works correctly for UCLAMP_MAX (a limit) but fails for UCLAMP_MIN, which should function as a protection where tasks are boosted up to the minimum, not capped down to it. Additionally, the `!uc_req.user_defined` condition caused RT tasks with default boosts to incorrectly change their behavior when attached to cgroups.

## Fix Summary

The fix differentiates between UCLAMP_MIN and UCLAMP_MAX in a switch statement: for UCLAMP_MIN, it checks if the task's value is below the group minimum and applies the minimum as a floor (protection); for UCLAMP_MAX, it checks if the task's value exceeds the group maximum and applies it as a ceiling (limit). It also removes the `!uc_req.user_defined` condition, simplifying the logic and preventing unintended side effects on tasks with default clamp values.

## Triggering Conditions

The bug occurs in `uclamp_tg_restrict()` when tasks with per-task uclamp.min values are attached to cgroups that have cpu.uclamp.min configured. The critical path involves the uclamp restriction logic during task group attachment or uclamp updates. Specifically: a task with uclamp.min lower than the cgroup's cpu.uclamp.min should be boosted to the cgroup minimum (protection semantics), but the buggy implementation incorrectly caps it down. Tasks with higher uclamp.min values than the cgroup minimum should retain their values but were incorrectly reduced. The bug also affects RT tasks with default boost values when the `!uc_req.user_defined` condition triggers incorrect overrides. No special CPU topology or timing conditions are required.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved). In setup(), create a cgroup with `kstep_cgroup_create("test_group")` and set its cpu.uclamp.min to 20% using `kstep_cgroup_write("test_group", "cpu.uclamp.min", "20")`. Create two CFS tasks: one with default uclamp.min=0 and another with explicit uclamp.min=50% via uclamp syscalls or proc writes. In run(), attach both tasks to the test_group using `kstep_cgroup_add_task()`. Monitor the effective uclamp values by reading /proc/[pid]/sched or adding kernel logging in uclamp_tg_restrict(). The bug is triggered when the first task shows effective uclamp.min=0 (should be 20%) and the second task shows effective uclamp.min=20% (should remain 50%). Use `on_tick_begin()` callback to log uclamp states and detect incorrect restriction behavior.
