# sched,init: Fix DEBUG_PREEMPT vs early boot

- **Commit:** 15faafc6b449777a85c0cf82dd8286c293fed4eb
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

During early boot, the init task must be moved from CPU 0 to a non-isolated CPU via `set_cpus_allowed_ptr()`. However, the init task has the `PF_NO_SETAFFINITY` flag set. When `is_percpu_thread()` was introduced (in commit 570a752b7a9b) to check this flag instead of `nr_cpus_allowed`, it prevented the CPU affinity change from taking effect, causing the init task to remain pinned to CPU 0. This breaks systems with isolated CPUs and causes DEBUG_PREEMPT checks to fail during early boot.

## Root Cause

The init task is initialized with `PF_NO_SETAFFINITY` to prevent userspace from changing its affinity during bootup. However, the kernel itself needs to move init away from CPU 0 during `sched_init_smp()`. After the commit that introduced `is_percpu_thread()` checking the `PF_NO_SETAFFINITY` flag, this internal kernel operation was blocked, leaving the flag set when it should have been cleared after the move.

## Fix Summary

After successfully moving the init task to a non-isolated CPU in `sched_init_smp()`, the `PF_NO_SETAFFINITY` flag is cleared via `current->flags &= ~PF_NO_SETAFFINITY`. This allows the init task to run freely on the allowed CPUs while preventing userspace interference during early boot.

## Triggering Conditions

The bug occurs during early boot initialization when:
- The init task (PID 1) has `PF_NO_SETAFFINITY` flag set in `rest_init()`
- `sched_init_smp()` calls `set_cpus_allowed_ptr()` to move init from CPU 0 to non-isolated CPUs
- `is_percpu_thread()` checks the `PF_NO_SETAFFINITY` flag instead of `nr_cpus_allowed`
- The flag remains set, blocking the affinity change and leaving init pinned to CPU 0
- Systems with isolated CPUs or DEBUG_PREEMPT configurations trigger failures
- The bug manifests as DEBUG_PREEMPT checks failing when init tries to run on CPU 0

## Reproduce Strategy (kSTEP)

This bug occurs during early kernel boot and involves the init task migration, making it challenging to reproduce with kSTEP's user-mode drivers. However, a potential approach:
- Configure a multi-CPU system (at least 3 CPUs: CPU 0 reserved, CPU 1+ for tasks)
- Use `kstep_topo_init()` and `kstep_topo_apply()` to simulate isolated CPU topology
- Create a task that mimics init behavior: `kstep_task_create()` with custom PF_NO_SETAFFINITY flag manipulation
- Pin the task to CPU 0: `kstep_task_pin(task, 0, 0)`
- Attempt affinity change via internal scheduler state modification to trigger `is_percpu_thread()` check
- Use `on_tick_begin` callback to monitor task CPU assignments and detect stuck migration
- Check if the task remains on CPU 0 despite attempted migration to verify bug reproduction
