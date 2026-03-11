# sched/deadline: Create DL BW alloc, free & check overflow interface

- **Commit:** 85989106feb734437e2d598b639991b9185a43a6
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/sched.h
- **Subsystem:** Deadline

## Bug Description

When moving a set of deadline (DL) tasks between exclusive cpusets, the kernel would allocate deadline bandwidth (DL BW) on the destination root_domain for each task. However, if an error occurred during this process (either DL BW overflow checking failed for a subsequent task, or another cgroup controller failed in its can_attach() hook), the already-allocated DL BW would not be freed. This could leave the scheduler in an inconsistent state with leaked BW allocations, preventing future tasks from attaching to cpusets.

## Root Cause

The old `dl_cpu_busy()` function performed both overflow checking and BW allocation in a single call that could not be partially rolled back. Without a separate free function, when `task_can_attach()` called `dl_cpu_busy()` for multiple tasks or when a subsequent cgroup controller failed, there was no mechanism to release the BW that had already been allocated to tasks in the earlier iterations of the loop.

## Fix Summary

The fix splits the monolithic `dl_cpu_busy()` function into three separate functions: `dl_bw_check_overflow()` for overflow checking only, `dl_bw_alloc()` for allocation, and `dl_bw_free()` for freeing. These functions take a `u64 dl_bw` parameter instead of a task struct, allowing the caller to manage allocations and rollback on error properly.

## Triggering Conditions

This bug occurs during cpuset migration of multiple deadline tasks when `task_can_attach()` calls the old `dl_cpu_busy()` for each task in sequence. The bug is triggered when: (1) Multiple DL tasks are being moved to an exclusive cpuset, (2) DL bandwidth overflow checking fails for a subsequent task after some tasks already had their BW allocated, or (3) Another cgroup controller (e.g., memory, pids) fails its `can_attach()` hook after DL BW allocation succeeds. The leaked bandwidth remains allocated on the destination root_domain, preventing future task migrations and leaving the scheduler in an inconsistent state.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs to create exclusive cpusets. Create multiple DL tasks with high bandwidth requirements using `kstep_task_create()` and deadline parameters. Create two exclusive cpusets with `kstep_cgroup_create()` and `kstep_cgroup_set_cpuset()` (e.g., "cpuset1" on CPU 1, "cpuset2" on CPU 2). Place DL tasks in cpuset1 initially with `kstep_cgroup_add_task()`. Set DL bandwidth limits low enough that moving all tasks would cause overflow. Attempt to move tasks to cpuset2 by writing to cgroup.procs, which should trigger `task_can_attach()` and cause the first task's BW to be allocated but overflow to occur on the second task. Monitor DL bandwidth allocations via `/proc/sys/kernel/sched_rt_*` or internal scheduler state to detect leaked allocations that aren't properly freed when the operation fails.
