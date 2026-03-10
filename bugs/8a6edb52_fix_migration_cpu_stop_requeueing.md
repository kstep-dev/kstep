# sched: Fix migration_cpu_stop() requeueing

- **Commit:** 8a6edb5257e2a84720fe78cb179eca58ba76126f
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When `affine_move_task()` calls `stop_one_cpu()` to migrate a running task, and that task concurrently migrates to a different CPU (e.g., via load balancing), the function attempts to requeue the migration stop work using `pending->arg`. However, `pending->arg` is never initialized, leaving it as all zeros, which causes `migration_cpu_stop()` to execute with `arg->p == NULL`. This results in a crash or undefined behavior.

## Root Cause

The `affine_move_task()` function creates a local `migration_arg` structure but does not initialize the `pending->arg` field when reissuing the migration callback via `stop_one_cpu_nowait()`. When a concurrent migration occurs and the function takes the branch to requeue the stop work, it passes the uninitialized `pending->arg` to `migration_cpu_stop()`, which then operates on a NULL task pointer.

## Fix Summary

The fix initializes `pending->arg` during the setup of the pending affinity structure in `affine_move_task()`, ensuring it contains valid task and destination CPU information. It also replaces `stop_one_cpu()` with `stop_one_cpu_nowait()` using the initialized `pending->arg`, and adds a check in `migration_cpu_stop()` to distinguish between different use cases based on whether `arg->pending` is set.

## Triggering Conditions

This bug requires a precise race condition in the scheduler's task migration path:
- A task must be running on a CPU when `affine_move_task()` is called (e.g., via `sched_setaffinity()`)
- The `affine_move_task()` function initiates migration by calling `stop_one_cpu()` with `migration_cpu_stop()`
- Before the migration stop work executes, the task must concurrently migrate to a different CPU due to load balancing
- When `migration_cpu_stop()` runs and finds the task is no longer on the expected runqueue, it takes the `else if (dest_cpu < 1 || pending)` branch
- The function then tries to requeue the migration using `stop_one_cpu_nowait(&pending->arg)`, but `pending->arg` was never initialized
- This results in `migration_cpu_stop()` being called with `arg->task == NULL`, causing a null pointer dereference

## Reproduce Strategy (kSTEP)

Use a 3-CPU setup (CPU 0 reserved for driver, CPUs 1-2 available):
- In `setup()`: Create two tasks and configure asymmetric load to encourage load balancing between CPUs 1-2
- In `run()`: Pin one task to CPU 1 and let it run to establish load; pin another task to CPU 2 with higher load
- Use `kstep_tick_repeat()` to allow load balancing to activate between the CPUs
- Immediately trigger affinity change on the CPU 1 task using `kstep_cgroup_set_cpuset()` or direct affinity manipulation
- Use `on_tick_begin()` callback to monitor task migrations and detect when concurrent migration occurs
- In the callback, check if `migration_cpu_stop()` is called with invalid arguments by logging task pointer values
- The bug manifests as a kernel panic or null pointer dereference when the uninitialized `pending->arg` is used
- Success criteria: Observe the race condition where affinity change and load balancing migration overlap, leading to the crash
