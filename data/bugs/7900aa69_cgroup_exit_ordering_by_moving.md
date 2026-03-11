# sched_ext: Fix cgroup exit ordering by moving sched_ext_free() to finish_task_switch()

- **Commit:** 7900aa699c34401cf5d0c701d9ef72880ddc1a83
- **Affected file(s):** kernel/sched/core.c, kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

When a task exits, the scheduler must properly manage cgroup operations for extended scheduler classes (sched_ext). Two incorrect orderings could occur: (1) ops.init_task() could be called on a cgroup that was never initialized via ops.cgroup_init() because the cgroup was destroyed while a dead task still lingered on the scx_tasks list; (2) ops.cgroup_exit() could be called before ops.exit_task() was called on all member tasks, violating the required operation ordering. This causes incorrect cgroup state and potential crashes in sched_ext schedulers.

## Root Cause

sched_ext_free() was being called from __put_task_struct() when the last reference to a task was dropped, which could occur long after the task finished running. This temporal disconnect allowed cgroups to be destroyed while dead tasks remained visible on the scx_tasks list, causing the scheduler to see inconsistent state during initialization and exit operations.

## Fix Summary

Move sched_ext_free() from __put_task_struct() to finish_task_switch(), renaming it to sched_ext_dead(). Call it immediately after the task's final context switch completes, right before cgroup_task_dead(). This ensures tasks are removed from scx_tasks and have exit_task() called before any cgroup destruction occurs, maintaining proper operation ordering and validity of cgroup references.

## Triggering Conditions

The bug requires sched_ext tasks in cgroups where task exit cleanup is delayed relative to cgroup destruction. Specifically: (1) Tasks with SCHED_EXT policy must be placed in cgroups and added to the global scx_tasks list; (2) Tasks must exit/die but remain referenced (zombie state) so __put_task_struct() is delayed; (3) During this window, the cgroup containing the dead tasks must be destroyed, triggering ops.cgroup_exit() while dead tasks still appear on scx_tasks; (4) Alternatively, new tasks can be moved into the destroyed cgroup, causing ops.init_task() calls on uninitialized cgroups. The race occurs in the window between task death and final reference drop when cgroup_mutex allows cgroup destruction despite lingering dead tasks.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. In setup(), create a test cgroup with kstep_cgroup_create("test_cgroup"). Create multiple tasks with kstep_task_create() and add them to the test cgroup using kstep_cgroup_add_task(). In run(), force tasks to exit by calling kstep_task_pause() to simulate task death while maintaining references. During the exit window, attempt to destroy and recreate the cgroup to trigger the race condition. Use on_tick_end() callback to monitor task states and log when tasks remain visible on scx_tasks list after cgroup operations. Detection requires checking if ops.cgroup_exit() occurs before all member tasks have completed ops.exit_task(), or if ops.init_task() is called on cgroups that lack proper initialization. Log cgroup lifecycle events and task exit ordering to identify the violation.
