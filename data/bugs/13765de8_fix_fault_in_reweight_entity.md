# sched/fair: Fix fault in reweight_entity

- **Commit:** 13765de8148f71fa795e0a6607de37c49ea5915a
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** fair (CFS)

## Bug Description

A race condition exists between `sched_post_fork()` and `setpriority(PRIO_PGRP)` within a thread group that causes a null pointer dereference (GPF) in `reweight_entity()`. When a main process spawns new threads that call `setpriority(PRIO_PGRP)` before `sched_post_fork()` has completed initialization, `set_load_weight()` attempts to access the run queue pointer which has not yet been set, resulting in a kernel crash.

## Root Cause

A prior commit (4ef0c5c6b5ba) moved the initialization of the `cfs_rq` pointer from `sched_fork()` (called early in `copy_process()` before the task is added to the thread group) to `sched_post_fork()` (called after the task is added). This creates a window where `setpriority()` and `set_one_prio()` can be called on a newly created task that hasn't been fully initialized by `sched_post_fork()` yet, causing `reweight_entity()` to dereference a NULL pointer.

## Fix Summary

The fix modifies `set_load_weight()` to check the `TASK_NEW` flag internally before attempting to reweight the task. Instead of accepting an `update_load` parameter, the function now computes it by checking `!(READ_ONCE(p->__state) & TASK_NEW)`, ensuring that tasks still in the process of being forked are not reweighted, avoiding the null pointer dereference.

## Triggering Conditions

This bug requires a race between task creation and priority changes within a thread group:
- A main process spawns new threads that join the same thread group (CLONE_THREAD)
- Before `sched_post_fork()` completes initialization for new tasks, another thread calls `setpriority(PRIO_PGRP)` 
- The `setpriority()` call triggers `set_one_prio()` → `set_user_nice()` → `set_load_weight()` → `reweight_entity()`
- Tasks in TASK_NEW state have NULL cfs_rq pointers, causing reweight_entity() to dereference NULL
- Timing window exists between `copy_process()` adding task to thread group and `sched_post_fork()` setting cfs_rq
- Higher likelihood with multiple threads being spawned rapidly and setting priorities concurrently

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved). Create a main process with multiple child tasks that race between fork and setpriority:
- In setup(): Create parent task and 3-4 child tasks using `kstep_task_create()`
- Use `kstep_task_fork()` on parent to spawn multiple children rapidly within same thread group
- Immediately after forking, call `kstep_task_set_prio()` on various tasks with different nice values
- Monitor for GPF/NULL deref in kernel logs via `on_tick_begin()` callback with custom logging
- Check task `__state` for TASK_NEW flag and cfs_rq pointer status to detect vulnerable window
- Reproduce by repeating fork+setprio sequence with tight timing, potentially using `kstep_tick()` to control scheduling
- Bug triggered when `reweight_entity()` accesses NULL cfs_rq during priority change on incompletely initialized task
