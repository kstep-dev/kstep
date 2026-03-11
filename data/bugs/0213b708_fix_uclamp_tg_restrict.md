# sched/uclamp: Fix uclamp_tg_restrict()

- **Commit:** 0213b7083e81f4acd69db32cb72eb4e5f220329a
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The `uclamp_tg_restrict()` function incorrectly handles corner cases where a task's uclamp request conflicts with its taskgroup's uclamp bounds. This can result in invalid effective uclamp values where the minimum exceeds the maximum, or where the task's minimum falls below the taskgroup's minimum. For example, a task requesting min=60% with a taskgroup max=50% would incorrectly result in effective min=60%, max=50% (violating min ≤ max invariant).

## Root Cause

The original implementation only checked one side of the comparison: it would return the taskgroup's bound if the task's request exceeded it (for UCLAMP_MAX) or fell below it (for UCLAMP_MIN), but failed to properly clamp the task's value within both bounds simultaneously. This resulted in effective values that didn't respect both the taskgroup's minimum and maximum constraints at the same time.

## Fix Summary

The fix replaces the switch-case logic with a proper two-sided `clamp()` operation that simultaneously bounds the task's uclamp value between the taskgroup's minimum and maximum. Additionally, `uclamp_update_active()` now updates both UCLAMP_MIN and UCLAMP_MAX unconditionally, since changing one can affect the other's effective value.

## Triggering Conditions

The bug requires a uclamp-enabled kernel with CONFIG_UCLAMP_TASK_GROUP=y and tasks attached to cgroups with conflicting uclamp bounds. Specifically:
- A cgroup with uclamp.min/max configured (not the root task group)
- Tasks with individual uclamp requests that conflict with the cgroup bounds
- Two trigger cases: (1) task min > cgroup max, or (2) task max < cgroup min
- The bug manifests in `uclamp_tg_restrict()` during task enqueue/dequeue operations
- Results in effective uclamp values where min > max, violating the fundamental uclamp invariant
- Most observable during scheduler operations like task wakeup, migration, or priority changes

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). In setup(): create two cgroups with `kstep_cgroup_create("test1")` and `kstep_cgroup_create("test2")`. Set cgroup uclamp bounds using `kstep_cgroup_write("test1", "cpu.uclamp.min", "30")` and `kstep_cgroup_write("test1", "cpu.uclamp.max", "50")`. Create tasks with `kstep_task_create()` and set conflicting uclamp requests via sysfs writes or task attributes. In run(): add tasks to cgroups with `kstep_cgroup_add_task()`, then trigger scheduler operations with `kstep_task_wakeup()` and `kstep_tick()`. Use custom logging to read task effective uclamp values from `/sys/fs/cgroup/test1/cpu.uclamp.min` and verify they violate min ≤ max invariant. Observe via callback functions during uclamp updates and task state changes.
