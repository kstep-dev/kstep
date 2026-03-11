# sched/fair: Allow changing cgroup of new forked task

- **Commit:** df16b71c686cb096774e30153c9ce6756450796c
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c
- **Subsystem:** CFS (Fair Scheduling)

## Bug Description

Previously, attempting to change the cgroup of a newly forked task would fail with -EINVAL in the `cpu_cgroup_can_attach()` function. This prevented users from assigning new tasks to different cgroups before they were woken up, an artificial limitation that had no valid technical reason to exist.

## Root Cause

The limitation was introduced to prevent calling `task_change_group_fair()` on tasks in the TASK_NEW state, before `wake_up_new_task()` has been called. The old code assumed this would cause a "detach on an unattached task sched_avg problem" because detach/attach operations would operate on uninitialized PELT state. However, the detach/attach operations could be safely skipped for TASK_NEW tasks entirely, making the blanket rejection unnecessary.

## Fix Summary

The fix moves the TASK_NEW check from `cpu_cgroup_can_attach()` to `task_change_group_fair()`, where it skips the detach/attach operations for TASK_NEW tasks. This allows cgroup changes for new forked tasks while safely avoiding the problematic PELT operations. The `cpu_cgroup_can_attach()` function is now conditionally compiled only for CONFIG_RT_GROUP_SCHED.

## Triggering Conditions

The bug is triggered in the CFS cgroup attachment path when attempting to move a newly forked task to a different cgroup before `wake_up_new_task()` has been called. The task must be in TASK_NEW state, which occurs immediately after `copy_process()` but before the task is woken up. The `cpu_cgroup_can_attach()` function would check the task state under pi_lock and return -EINVAL for TASK_NEW tasks, preventing legitimate cgroup changes. This creates a race window between task creation and first wakeup where cgroup migration fails unnecessarily.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). In `setup()`, create two cgroups using `kstep_cgroup_create("group1")` and `kstep_cgroup_create("group2")`. In `run()`, create a parent task with `kstep_task_create()` and assign it to group1 using `kstep_cgroup_add_task("group1", parent->pid)`. Fork the parent task using `kstep_task_fork(parent, 1)` to create a child in TASK_NEW state. Immediately attempt to move the child to group2 with `kstep_cgroup_add_task("group2", child->pid)` before calling `kstep_task_wakeup(child)`. Use `on_sched_softirq_end()` callback to detect cgroup attachment failures via `kstep_fail()` if -EINVAL is returned, indicating the bug was triggered.
