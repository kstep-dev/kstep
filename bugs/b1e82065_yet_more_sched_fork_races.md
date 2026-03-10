# sched: Fix yet more sched_fork() races

- **Commit:** b1e8206582f9d680cff7d04828708c8b6ab32957
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition exists in task fork initialization: when a previous fix (commit 4ef0c5c6b5ba) prevented cgroup-related issues by deferring certain initialization, it inadvertently exposed the task through the pidhash (making it visible to syscalls) before the task was added to the runqueue. This creates a window where other syscalls or kernel code can interact with the forked task's scheduler state while it is not yet properly initialized, leading to potential memory access violations or scheduler state corruption.

## Root Cause

The problem stems from the order of operations during task fork. Previously, the cgroup-related initialization (which requires holding locks and accessing potentially uninitialized sched_task_group) was performed in sched_post_fork(), which was called *after* the task became visible in the pidhash. Additionally, the set_load_weight() function determined whether to update load weights based on checking the TASK_NEW flag at the time of call, which is unreliable when the task becomes visible to other code paths before being fully initialized. The task needs to be fully prepared (including runqueue placement and cgroup association) *before* it becomes visible.

## Fix Summary

The fix reorders task initialization by splitting sched_post_fork() into two functions: sched_cgroup_fork() (which handles cgroup initialization and must be called before task visibility) and sched_post_fork() (called after). It also makes set_load_weight() take an explicit update_load parameter instead of deriving it from the TASK_NEW flag, ensuring deterministic behavior regardless of task visibility state. Callers explicitly specify whether load weight updating is appropriate for their context.

## Triggering Conditions

The race occurs in the task fork path when a newly created task becomes visible in the pidhash before being properly added to the runqueue and having its cgroup associations initialized. The vulnerable window exists between `wake_up_new_task()` making the task visible and `sched_post_fork()` completing the scheduler initialization. Concurrent syscalls (like `sched_getaffinity()`, `sched_setscheduler()`, or cgroup operations) accessing the task during this window can trigger access to uninitialized scheduler state. The race is more likely under heavy fork load, multi-CPU systems, and when cgroups are actively used. The bug manifests as memory access violations, inconsistent load weight calculations, or scheduler state corruption when these syscalls interact with tasks that have incomplete scheduler initialization.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Setup should create multiple cgroups using `kstep_cgroup_create()` and `kstep_cgroup_set_weight()` with different weights to make load weight calculations sensitive to initialization order. In `run()`, rapidly fork multiple tasks using `kstep_task_fork()` on different CPUs while simultaneously triggering scheduler-related operations that would access the newly created tasks. Use `kstep_task_create()` followed by immediate `kstep_task_pin()` to different CPUs to simulate concurrent access during the race window. Implement `on_tick_begin()` and `on_tick_end()` callbacks to log scheduler state changes and detect inconsistencies in task state, cgroup associations, or load weights. The bug manifests as crashes during load weight calculations or inconsistent scheduler state when tasks are accessed before full initialization completes.
