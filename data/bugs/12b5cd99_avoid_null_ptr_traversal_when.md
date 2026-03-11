# sched/ext: Avoid null ptr traversal when ->put_prev_task() is called with NULL next

- **Commit:** 12b5cd99a05f7cbc2ceb88b3b9601d404ef2236a
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

In `put_prev_task_scx()`, the code calls `sched_class_above(&ext_sched_class, next->sched_class)` without checking if `next` is NULL. When `put_prev_task()` is invoked with a NULL next parameter (which can happen in certain scenarios, particularly with proxy-exec), this causes a NULL pointer dereference and kernel crash. The issue manifests as a NULL pointer traversal when trying to access the `sched_class` field of a NULL task pointer.

## Root Cause

The `put_prev_task()` function can legitimately be called with a NULL `next` parameter, but the code in `put_prev_task_scx()` assumes `next` is always valid when evaluating the `sched_class_above()` condition. There was already a NULL check for `next` in the `switch_class` label further down, but the earlier check on line 2404 lacked this defensive guard, creating a potential NULL dereference window.

## Fix Summary

The fix adds a simple NULL check before the `sched_class_above()` call: `if (next && sched_class_above(...))`. This prevents dereferencing `next` when it is NULL, making the code defensive against all possible invocation patterns of `put_prev_task()`.

## Triggering Conditions

The bug occurs in the sched_ext (EXT) scheduler subsystem, specifically in `put_prev_task_scx()`. To trigger this bug, the following conditions must be met:
- The current task must be managed by sched_ext (have `SCX_TASK_QUEUED` flag set)
- The `put_prev_task()` function must be called with a NULL `next` parameter
- This happens when switching away from an ext task but no next task is specified
- Originally observed with proxy-exec interactions, but can potentially occur in other scenarios
- The task being put must be runnable and queued to reach the problematic code path
- A remaining `put_prev_task()` call in core.c could potentially trigger this condition

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce with kSTEP since it requires sched_ext subsystem which may not be fully supported by the current framework. A hypothetical approach would be:
- Requires kernel with sched_ext enabled and configured
- Need at least 2 CPUs (CPU 0 reserved for driver)
- Setup would require enabling sched_ext and creating ext-managed tasks
- Use `kstep_task_create()` to create tasks that would be managed by sched_ext
- Trigger scenarios where tasks are being preempted or switched without a clear next task
- Monitor for NULL pointer dereference in `put_prev_task_scx()` via kernel logs
- Detection would rely on kernel crash logs showing NULL dereference at the specific line
- Since sched_ext support in kSTEP may be limited, this reproduction might require framework extensions or may not be feasible with current kSTEP capabilities
