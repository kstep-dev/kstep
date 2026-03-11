# Avoid double calling update_rq_clock() in __balance_push_cpu_stop()

- **Commit:** 96500560f0c73c71bca1b27536c6254fa0e8ce37
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A double invocation of `update_rq_clock()` was occurring in the task migration path. The function was being called once in `__balance_push_cpu_stop()` and again inside the `__migrate_task()` function it calls, leading to redundant clock updates that waste CPU cycles and could potentially cause incorrect scheduler state transitions or timing inconsistencies during CPU hotplug or task affinity changes.

## Root Cause

The `__migrate_task()` function unconditionally called `update_rq_clock()` at the beginning, but all its callers were also calling `update_rq_clock()` before invoking it. This created a duplicate clock update in the common task migration path. While multiple clock updates may not cause data corruption, they represent wasted cycles and unnecessary state modifications when a single update would suffice.

## Fix Summary

The fix removes the `update_rq_clock()` call from inside `__migrate_task()` and ensures that the caller `migration_cpu_stop()` explicitly calls `update_rq_clock()` before invoking `__migrate_task()`, eliminating the double update while maintaining correct clock state for the migration operation.

## Triggering Conditions

This bug occurs during task migration operations in the core scheduler subsystem, specifically when:
- A task needs to be migrated from one CPU to another due to CPU hotplug events or task affinity changes
- The `__balance_push_cpu_stop()` function is invoked, which calls `update_rq_clock()` then `__migrate_task()`
- Inside `__migrate_task()`, another redundant `update_rq_clock()` call occurs before calling `move_queued_task()`
- The double clock update happens in the migration path regardless of task state - any runnable task being migrated triggers this
- No specific timing conditions are required; the bug occurs deterministically on every task migration through this path
- The issue manifests as wasted CPU cycles during migration operations, particularly noticeable under frequent CPU hotplug or affinity changes

## Reproduce Strategy (kSTEP)

Use a multi-CPU setup (minimum 2 CPUs beyond CPU 0) to trigger task migrations:
- In `setup()`: Create 2-3 tasks with `kstep_task_create()` and pin them to specific CPUs using `kstep_task_pin()`
- In `run()`: Wake up tasks on CPU 1 with `kstep_task_wakeup()`, let them run with `kstep_tick_repeat(10)`
- Force migrations by changing task affinity: call `kstep_task_pin()` to move tasks from CPU 1 to CPU 2
- Use `kstep_tick_repeat(5)` to allow migration to complete
- Instrument the kernel to count `update_rq_clock()` calls during migration (add printk or use tracing)
- In callbacks like `on_tick_end()`, monitor for migration events and log clock update frequency
- Detection: Compare clock update counts before/after the fix - buggy kernel shows 2 updates per migration, fixed kernel shows 1
- Alternative: Hook `__migrate_task()` and `__balance_push_cpu_stop()` to directly count `update_rq_clock()` invocations
