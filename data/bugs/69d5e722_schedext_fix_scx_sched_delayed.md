# sched/ext: Fix scx vs sched_delayed

- **Commit:** 69d5e722be949a1e2409c3f2865ba6020c279db6
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext / eBPF scheduler)

## Bug Description

When switching tasks between scheduler classes during SCX (eBPF scheduler) enable/disable operations, tasks with delayed dequeuing semantics were not properly dequeued before the scheduler class transition. This could leave a task in a delay-dequeued state after switching back to a different scheduler class (e.g., fair), causing incorrect task queueing state and triggering warnings when the scheduler expects the task to not be delayed-dequeued.

## Root Cause

A previous commit (98442f0ccd82) fixed the delayed dequeue issue in `switched_from_fair()` by explicitly dequeuing tasks with `DEQUEUE_DELAYED` before changing scheduler classes, but this fix was not applied to the SCX code paths in `scx_ops_disable_workfn()` and `scx_ops_enable()`. When switching scheduler classes, these functions did not check for the `p->se.sched_delayed` flag, leaving delayed-dequeued tasks in an inconsistent state.

## Fix Summary

The fix adds explicit dequeue calls with `DEQUEUE_DELAYED` flag before changing scheduler classes in both `scx_ops_disable_workfn()` and `scx_ops_enable()`. It checks if `old_class != new_class && p->se.sched_delayed` and, if true, explicitly dequeues the task with `DEQUEUE_SLEEP | DEQUEUE_DELAYED` flags before proceeding with the scheduler class change. This mirrors the fix applied to the regular scheduler transition code and ensures delayed-dequeued tasks are properly handled during SCX transitions.

## Triggering Conditions

The bug occurs when SCX (sched_ext) scheduler operations switch tasks between scheduler classes while a task has delayed dequeuing enabled (`p->se.sched_delayed` is set). Specifically:
- A task must be in the fair scheduler class with delayed dequeuing active (e.g., task was running and got delayed-dequeued to maintain temporal fairness)
- SCX enable/disable operations (`scx_ops_disable_workfn()` or `scx_ops_enable()`) must transition the task to a different scheduler class
- The timing window occurs during scheduler class transitions where `old_class != new_class` but the delayed dequeue state is not cleared
- The bug manifests when the scheduler later expects the task to not be delayed-dequeued but finds `p->se.sched_delayed` still set

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create a fair-scheduled task that gets delayed-dequeued, then simulate SCX class transitions:

1. **Setup:** Create a single CFS task with `kstep_task_create()` and `kstep_task_cfs()`
2. **Get delayed state:** Run the task with `kstep_task_wakeup()`, then use `kstep_tick_repeat()` to let it accumulate runtime and get delayed-dequeued
3. **Force sched_delayed:** Use direct manipulation to set `task->se.sched_delayed = 1` (simulating delayed dequeue state)
4. **Simulate SCX transition:** Directly call scheduler class change logic or manipulate `task->sched_class` to simulate SCX disable/enable operations
5. **Detection:** Use `on_tick_begin()` callback to log task state and check if `task->se.sched_delayed` remains set after class transition
6. **Verification:** Attempt to enqueue/dequeue the task and monitor for scheduler warnings or inconsistent state through `kstep_print_sched_debug()`
