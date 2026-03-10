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

## Triggering Conditions

The bug requires a precise race condition during task forking with concurrent cgroup operations:
- Multiple cgroups with different `sched_task_group` configurations must exist
- A parent task must initiate fork() to create a child process
- Concurrently, the parent task must be moved to a different cgroup, causing the old cgroup and its `sched_task_group` to be freed
- The timing window is between `dup_task_struct()` (where child copies parent's stale `sched_task_group` pointer) and `sched_fork()` (where freed memory is accessed)
- The race manifests when `sched_fork()` calls `__set_task_cpu()` and `task_fork_fair()`, which dereference the child's now-invalid `sched_task_group`
- Multi-CPU systems increase the likelihood of this race condition occurring

## Reproduce Strategy (kSTEP)

Configure a multi-CPU environment (minimum 2 CPUs) and create competing cgroups to trigger the race:
- In `setup()`: Create two cgroups ("cgroup_a" and "cgroup_b") with different weights using `kstep_cgroup_create()` and `kstep_cgroup_set_weight()`
- Create a parent task with `kstep_task_create()` and assign it to "cgroup_a" using `kstep_cgroup_add_task()`
- In `run()`: Use `kstep_task_fork(parent, 1)` to initiate child creation, immediately followed by `kstep_cgroup_add_task()` to move parent to "cgroup_b"
- Repeat this sequence in a tight loop to increase race condition probability
- Use `on_tick_begin()` callback to monitor for kernel panics or use-after-free symptoms
- Check for crash signatures like "unable to handle kernel NULL pointer dereference" in `sched_slice()` or related scheduler functions
- Log cgroup membership changes and fork operations to correlate timing with any detected crashes
