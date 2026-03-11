# sched: Fix migrate_disable_switch() locking

- **Commit:** 942b8db965006cf655d356162f7091a9238da94e
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The `migrate_disable_switch()` function had a known locking violation: it called `__do_set_cpus_allowed()` without holding the required locks, despite the code containing a comment acknowledging "Violates locking rules!" This violated the affinity locking protocol which requires holding both `rq->lock` and `p->pi_lock` when modifying CPU affinity state. The violation relied on fragile assumptions about task state that could lead to subtle race conditions.

## Root Cause

The function was attempting to update task CPU affinity during the scheduler's context switch path in a location where the locking was incomplete and required workarounds. The code used a special case check (`if (ctx->flags & SCA_MIGRATE_DISABLE)`) that relied on the task being on-CPU rather than holding proper locks, creating a dubious locking pattern that violated normal affinity change rules.

## Fix Summary

The fix moves the `migrate_disable_switch()` call earlier in the scheduler, before `rq->lock` is acquired, and wraps the affinity update with proper `task_rq_lock` guards to ensure all required locks are held. This eliminates the locking violation, removes the special-case handling, and simplifies the code by following the normal locking protocol.

## Triggering Conditions

The bug occurs during task scheduling when a migration-disabled task undergoes a context switch. Specifically:
- A task must have `migration_disabled` set (typically via `migrate_disable()`)
- The task's `cpus_ptr` must point to `&cpus_mask` (not already restricted)
- A context switch must occur while the task is running on a CPU
- The scheduler calls `migrate_disable_switch()` in the critical section where `rq->lock` is held but `p->pi_lock` is not
- This creates a window where `__do_set_cpus_allowed()` violates the affinity locking protocol
- The race condition can manifest during concurrent affinity changes or wakeup operations that access the same task's affinity state

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Setup migration-disabled tasks and trigger context switches:
- Use `kstep_task_create()` to create multiple tasks and pin them to different CPUs with `kstep_task_pin()`
- Create a task that will call `migrate_disable()` by using kSTEP's task manipulation functions
- Force context switches through `kstep_tick_repeat()` while tasks have migration disabled
- Use `on_tick_begin` callback to monitor task states and scheduler activity
- Monitor for locking violations by checking task affinity state consistency across context switches
- Detect the bug by observing improper affinity updates during the vulnerable window in `__schedule()`
- Use `kstep_output_curr_task()` and custom logging to track when `migrate_disable_switch()` is called
- The bug manifests as potential data races in task affinity fields during concurrent scheduler operations
