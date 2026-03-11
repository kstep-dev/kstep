# Fix the bug that task won't enqueue into core tree when update cookie

- **Commit:** 91caa5ae242465c3ab9fd473e50170faa7e944f4
- **Affected file(s):** kernel/sched/core_sched.c
- **Subsystem:** core

## Bug Description

When a task's core scheduling cookie is updated via `sched_core_update_cookie()`, if the task is already running on the runqueue but has not yet been enqueued into the core tree (e.g., assigned a cookie for the first time), the task is not re-enqueued into the core tree. This causes the task to be treated as unrelated to other tasks with the same cookie, resulting in unnecessary force idle periods even when tasks should be paired as compatible.

## Root Cause

The original code uses a boolean variable `enqueued` that captures whether the task was in the core tree *before* the cookie update. When re-enqueueing the task after the cookie change, it only re-enqueues if `enqueued` was true. This misses the critical case where a task is already on the runqueue but not yet in the core tree (e.g., a previously uncookied task being assigned a cookie for the first time). The task should be enqueued into the core tree if it's on the runqueue and now has a cookie, regardless of whether it was previously in the core tree.

## Fix Summary

The fix changes the re-enqueue condition from `if (enqueued)` to `if (cookie && task_on_rq_queued(p))`, ensuring that a task is enqueued into the core tree whenever it is on the runqueue and has a cookie assigned, not just when it was previously in the core tree. This guarantees that tasks acquire proper core pairing immediately upon cookie assignment.

## Triggering Conditions

The bug occurs in the core scheduling subsystem when:
- Tasks are already running on SMT sibling CPUs without core scheduling cookies
- A cookie is assigned to some running tasks via `sched_core_update_cookie()` for the first time
- The newly cookied tasks remain on the runqueue but are not enqueued into the core tree
- Tasks with the same cookie on sibling CPUs experience unnecessary force idle periods
- The condition requires SMT topology with at least 2 sibling CPUs, running tasks that transition from uncookied to cookied state
- Race condition: task must be on runqueue when cookie is assigned but not previously in core tree

## Reproduce Strategy (kSTEP)

Need 2+ CPUs (CPU 0 reserved). Setup SMT topology with CPUs 1-2 as siblings:
- Use `kstep_topo_init()`, `kstep_topo_set_smt()` to create SMT pair [1,2]
- Create 3 tasks: `task_a`, `task_b`, `task_c` via `kstep_task_create()`
- Pin `task_a` to CPU 1, `task_b` and `task_c` to CPU 2 using `kstep_task_pin()`
- Run tasks with `kstep_task_wakeup()` and `kstep_tick_repeat()` to establish running state
- Assign same cookie to `task_a` and `task_b`, different cookie to `task_c` (simulate cgroup assignment)
- Monitor force idle behavior using `on_tick_begin()` callback to track CPU idle states
- Check if `task_a` experiences force idle despite sharing cookie with `task_b`
- Observe core tree enqueue/dequeue events to detect missing enqueue operations
- Bug triggered when cookied tasks force idle unnecessarily despite compatible cookies
