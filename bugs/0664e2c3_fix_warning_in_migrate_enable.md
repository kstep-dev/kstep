# sched/deadline: Fix warning in migrate_enable for boosted tasks

- **Commit:** 0664e2c311b9fa43b33e3e81429cd0c2d7f9c638
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline (real-time scheduler)

## Bug Description

When a deadline-scheduled task with priority boost is migrated via `migrate_enable()`, a WARN_ON is triggered in `setup_new_dl_entity()`. The warning occurs because the task is dequeued and re-enqueued with the ENQUEUE_RESTORE flag, which causes `setup_new_dl_entity()` to be called on a boosted task whose deadline parameters were already initialized by `rt_mutex_setprio()`, violating an assertion that expects parameters not to be reinitialized.

## Root Cause

Boosted tasks (those whose priority has been elevated by rt_mutex) have their deadline scheduling parameters already configured by `rt_mutex_setprio()`. During migration with ENQUEUE_RESTORE, the code path was checking if the deadline had expired and unconditionally calling `setup_new_dl_entity()` without verifying whether the task was boosted. This causes an attempt to reinitialize parameters that are already valid, triggering the WARN_ON check.

## Fix Summary

The fix adds a check `!is_dl_boosted(dl_se)` to the condition in the ENQUEUE_RESTORE path before calling `setup_new_dl_entity()`. This ensures boosted tasks skip reinitialization during migration, preserving their already-configured deadline parameters.

## Triggering Conditions

- A deadline task must be initially boosted via rt_mutex priority inheritance (e.g., waiting on a mutex held by a higher-priority task)
- The boosted task must have its deadline parameters already initialized by `rt_mutex_setprio()`
- A migration must be triggered on the boosted task via `migrate_enable()` or similar CPU affinity changes
- During migration, the task is dequeued and re-enqueued with the ENQUEUE_RESTORE flag set
- The task's deadline must appear expired relative to the current rq clock (`dl_time_before(dl_se->deadline, rq_clock())`)
- The code path in `enqueue_dl_entity()` reaches the ENQUEUE_RESTORE conditional without the `!is_dl_boosted()` check

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create a deadline task that gets priority-boosted through rt_mutex inheritance, then trigger migration:
- Use `kstep_task_create()` to create two deadline tasks: a mutex holder and a waiter
- Configure both as deadline tasks with `sched_setscheduler()` directly via kernel APIs
- Have the waiter task block on a mutex held by the holder task to trigger priority inheritance boost
- Use `kstep_task_pin()` to change the CPU affinity of the boosted waiter task to force migration
- Monitor for the WARN_ON trigger in `setup_new_dl_entity()` via `on_tick_begin()` callback
- Check task boost status with `is_dl_boosted()` and deadline expiry conditions
- Look for the warning message in kernel logs to detect successful reproduction
