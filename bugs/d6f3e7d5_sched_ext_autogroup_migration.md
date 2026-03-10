# sched_ext: Fix incorrect autogroup migration detection

- **Commit:** d6f3e7d564b2309e1f17e709a70eca78d7ca2bb8
- **Affected file(s):** kernel/sched/autogroup.c, kernel/sched/core.c, kernel/sched/ext.c, kernel/sched/ext.h, kernel/sched/sched.h
- **Subsystem:** EXT (sched_ext)

## Bug Description

`scx_move_task()` is called during task migration to notify the BPF scheduler of cgroup transitions. However, it was invoked for both cgroup and autogroup migrations. The function attempted to filter out autogroup migrations using heuristics (checking for autogroup task groups and the PF_EXITING flag), but these checks were insufficient. This caused the function to incorrectly ignore migrations from non-root cgroups to autogroups of the root cgroup, triggering a WARNING in `scx_cgroup_can_attach()` due to assertion failures.

## Root Cause

Without explicitly tagging the migration context in the call site, there is no reliable way to distinguish between a racing migration to the root cgroup and an autogroup migration. The previous heuristic approach using `task_group_is_autogroup()` and PF_EXITING checks could not handle all edge cases, particularly migrations to autogroups of the root cgroup, leading to incorrect filtering logic.

## Fix Summary

The fix adds a `for_autogroup` boolean parameter to `sched_move_task()` that explicitly indicates whether the migration is for autogroup purposes. When `for_autogroup` is true, the function skips invoking the ext scheduler migration handler. The function previously called `scx_move_task()` unconditionally; it now calls `scx_cgroup_move_task()` only when `for_autogroup` is false, ensuring the ext scheduler is only notified of actual cgroup migrations.

## Triggering Conditions

The bug occurs in the sched_ext subsystem during concurrent cgroup and autogroup migrations. Specifically:
- A task must be initially placed in a non-root cgroup (not the root task group)
- sched_ext scheduler must be active and monitoring cgroup transitions via `scx_move_task()`
- An autogroup migration must occur that moves the task to an autogroup belonging to the root cgroup
- The timing must create a race where `scx_move_task()` cannot distinguish between legitimate root cgroup migration and autogroup migration
- The heuristic checks (`task_group_is_autogroup()` and `PF_EXITING`) must fail to correctly identify the autogroup migration
- This causes `scx_move_task()` to incorrectly process the autogroup migration as a cgroup migration
- The incorrect processing triggers assertion failure in `scx_cgroup_can_attach()` leading to WARNING

## Reproduce Strategy (kSTEP)

To reproduce this bug using kSTEP (requires kernel with sched_ext support):
- Use at least 2 CPUs (CPU 0 reserved for driver)
- In `setup()`: Create tasks with `kstep_task_create()` and place them in non-root cgroups using `kstep_cgroup_create("testgroup")` and `kstep_cgroup_add_task("testgroup", task_pid)`
- Enable sched_ext scheduler via sysfs writes with `kstep_write()` to activate cgroup monitoring
- In `run()`: Trigger autogroup migration by manipulating process autogroup settings through `/proc/pid/autogroup`
- Use `kstep_tick_repeat()` to advance scheduler state and allow migration processing
- Monitor kernel logs with `on_sched_softirq_end()` callback to capture WARNING from `scx_cgroup_can_attach()`
- Detection: Look for "WARNING: CPU: X PID: Y at kernel/sched/ext.c:3725 scx_cgroup_can_attach" in logs
- The bug manifests as incorrect cgroup transition notification to sched_ext when autogroup migration should be filtered out
