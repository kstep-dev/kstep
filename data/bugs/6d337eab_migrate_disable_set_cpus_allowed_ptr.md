# sched: Fix migrate_disable() vs set_cpus_allowed_ptr()

- **Commit:** 6d337eab041d56bb8f0e7794f39906c21054c512
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition exists between concurrent calls to `migrate_disable()`, `migrate_enable()`, and `set_cpus_allowed_ptr()`. The bug violates the API contract that `set_cpus_allowed_ptr()` must not return until the task runs inside the provided affinity mask. When `migrate_enable()` attempts to restore the affinity mask while a `set_cpus_allowed_ptr()` call is pending, the synchronization fails, potentially leaving the task running on a forbidden CPU or causing incomplete task migration.

## Root Cause

The original code did not properly synchronize between `migrate_enable()` rewriting the affinity mask and concurrent `set_cpus_allowed_ptr()` calls. The simple barrier and early return in `migrate_enable()` (when `cpus_ptr == &cpus_mask`) did not account for the case where a `set_cpus_allowed_ptr()` call is in progress, and the code could not safely wait for completion due to preemption-disabled contexts (e.g., inside spinlocks). This created a window where affinity restoration could race with affinity changes, violating the API guarantee.

## Fix Summary

The fix introduces a `set_affinity_pending` structure to track async task migrations and store completion state. `migrate_enable()` now uses `preempt_disable()`/`preempt_enable()` to ensure atomicity when restoring affinity, and schedules the affinity restoration asynchronously via `stop_one_cpu_nowait()` when needed. A new `affine_move_task()` function consolidates the complex logic of handling concurrent affinity changes and migration disables, using refcounting to ensure proper completion signaling and prevent races between `migrate_enable()` restoring the mask and `set_cpus_allowed_ptr()` verifying task placement.

## Triggering Conditions

The bug requires a race between `migrate_enable()` restoring task affinity and a concurrent `set_cpus_allowed_ptr()` call. Key conditions:
- Task has migration disabled via `migrate_disable()` while running on a restricted CPU set
- A `set_cpus_allowed_ptr()` call is initiated from another thread while migration is disabled
- `migrate_enable()` attempts to restore the original `cpus_mask` while the affinity change is in progress
- The race window occurs when `migrate_enable()` checks `cpus_ptr == &cpus_mask` but a concurrent `set_cpus_allowed_ptr()` hasn't completed yet
- Task must be in a preemption-disabled or spinlock context that prevents `migrate_enable()` from waiting for completion
- The buggy code path involves task affinity restoration racing with pending affinity changes, violating the API guarantee

## Reproduce Strategy (kSTEP)

Target the race between `migrate_enable()` and `set_cpus_allowed_ptr()` using multi-CPU setup and kthread synchronization:
- Use 3+ CPUs (CPU 0 reserved for driver, test on CPUs 1-4)
- Create two kthreads: `task_main` (subject) and `task_setter` (affinity changer)
- In `setup()`: Use `kstep_kthread_create()` for both tasks, pin `task_main` initially to CPU 1
- In `run()`: Start `task_main` with `kstep_kthread_start()`, make it call `migrate_disable()` while running
- Use `kstep_kthread_syncwake()` to coordinate: `task_main` disables migration, signals `task_setter`
- `task_setter` calls `set_cpus_allowed_ptr()` to change `task_main`'s affinity to CPUs 2-3
- `task_main` immediately calls `migrate_enable()` to trigger the race
- Monitor with `on_tick_begin()` callback to log task CPU placement and affinity state
- Check for violation: task running outside its intended CPU mask after `set_cpus_allowed_ptr()` returns
- Use `TRACE_INFO()` to log migration state, CPU placement, and detect when API contract is violated
