# sched_ext: Fix incorrect assumption about migration disabled tasks in task_can_run_on_remote_rq()

- **Commit:** f3f08c3acfb8860e07a22814a344e83c99ad7398
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

A race condition occurs when trying to dispatch migration disabled tasks. The previous fix assumed a migration disabled task's `->cpus_ptr` would only contain the pinned CPU, but `->cpus_ptr` is updated via `migrate_disable_switch()` right before context switching. Since the task is enqueued earlier during `pick_next_task()`, another CPU can see the task on a DSQ and attempt to dispatch it before `->cpus_ptr` is restricted, causing `task_allowed_on_cpu()` to incorrectly succeed. This triggers a `SCHED_WARN()` warning when checking `is_migration_disabled()`, even though the migration disabled task should have been rejected.

## Root Cause

The timing of scheduler operations creates a race window: `put_prev_task_scx()` enqueues the task before `migrate_disable_switch()` updates `->cpus_ptr` to contain only the pinned CPU. During this window, another CPU can see the task on a DSQ and pass the `task_allowed_on_cpu()` check before the cpus_ptr is restricted, but fail at the migration disabled check, which was originally only a warning. The previous fix incorrectly relied on `task_allowed_on_cpu()` catching all migration disabled tasks, without accounting for this race condition.

## Fix Summary

The fix reorders the checks to test for migration disabled state before calling `task_allowed_on_cpu()`, catching migration disabled tasks early in the race window. It also converts the `SCHED_WARN()` into a regular error return path, allowing the function to gracefully reject migration disabled tasks without triggering kernel warnings. This ensures BPF schedulers that fail to handle migration disabled tasks properly are caught more reliably.

## Triggering Conditions

The bug requires a sched_ext (BPF) scheduler that uses dispatch queues (DSQs) to move tasks between CPUs. A race window occurs during task context switching: `put_prev_task_scx()` enqueues the migration disabled task on a DSQ during `pick_next_task()`, but `migrate_disable_switch()` doesn't update the task's `->cpus_ptr` until later during `__scheduler()`. In this narrow window, another CPU can see the task on the DSQ and attempt to dispatch it via `consume_dispatch_q()` or `scx_bpf_dsq_move_to_local()`. The `task_allowed_on_cpu()` check passes because `->cpus_ptr` still contains multiple CPUs, but `is_migration_disabled()` returns true, triggering the SCHED_WARN().

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (plus CPU 0 reserved). Setup two tasks: one migration-disabled on CPU 1, another that will trigger scheduler operations. Configure a sched_ext BPF scheduler that aggressively moves tasks between CPUs via DSQs. In `setup()`, create tasks with `kstep_task_create()` and pin the migration-disabled task with `kstep_task_pin(task, 1, 1)`. In `run()`, repeatedly call `kstep_tick()` while the migration-disabled task yields via `__do_sys_sched_yield()`, forcing context switches. Use `on_tick_begin()` callback to log task states and CPU assignments. Monitor for SCHED_WARN() warnings in kernel logs that indicate another CPU attempted to dispatch the migration-disabled task during the race window. Success is detecting the warning before the fix and its absence after.
