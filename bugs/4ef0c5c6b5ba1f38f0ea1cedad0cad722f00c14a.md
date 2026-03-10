# kernel/sched: Fix sched_fork() access an invalid sched_task_group

- **Commit:** 4ef0c5c6b5ba1f38f0ea1cedad0cad722f00c14a
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core scheduler

## Bug Description

A use-after-free race condition exists between `copy_process()` and `sched_fork()`. When a parent task forks a child, the child initially copies the parent's `sched_task_group`. However, if the parent is moved to another cgroup concurrently (freeing the old cgroup), the child's `sched_fork()` will access the freed `sched_task_group`, causing a kernel panic. The panic occurs when accessing freed memory in `sched_slice()` during task initialization.

## Root Cause

The root cause is a missing synchronization barrier. The child task's `sched_task_group` is set at `dup_task_struct()` in `copy_process()`, but the parent can be moved to another cgroup (freeing the old one) before `sched_fork()` is called. When `sched_fork()` then accesses the child's `sched_task_group` to call `__set_task_cpu()` and `task_fork()`, it dereferences freed memory. The cgroup membership can only be safely modified between `cgroup_can_fork()` and `cgroup_post_fork()`, but originally these operations happened outside that window.

## Fix Summary

The fix moves `__set_task_cpu()`, `rseq_migrate()`, and `task_fork()` operations from `sched_fork()` to the new `sched_post_fork()` function, which is called after `cgroup_post_fork()`. Additionally, `sched_post_fork()` now takes `kernel_clone_args` and explicitly reads the child's correct `sched_task_group` from the cgroup subsystem, ensuring the latest task group is used instead of relying on a stale copy.
