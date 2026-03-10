# sched: Move sched_class::prio_changed() into the change pattern

- **Commit:** 6455ad5346c9cf755fa9dda6e326c4028fb3c853
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/ext.c, kernel/sched/fair.c, kernel/sched/idle.c, kernel/sched/rt.c, kernel/sched/sched.h, kernel/sched/stop_task.c, kernel/sched/syscalls.c
- **Subsystem:** deadline scheduler, core scheduler

## Bug Description

The deadline scheduler's `prio_changed_dl()` function could not properly detect when a task's deadline changed because it lacked access to the old deadline value. The function received only the old generic task priority (`int oldprio`), not the deadline metric. This forced the scheduler to use a conservative fallback heuristic (`if (!rq->dl.overloaded) deadline_queue_pull_task(rq)`), resulting in incorrect decisions about whether to pull/push tasks when deadlines changed.

## Root Cause

The core issue was an architectural limitation: when `prio_changed()` callbacks were invoked, there was no mechanism to pass scheduler-specific priority metrics (like deadline for deadline tasks). Each scheduler class needed its own way to determine what "priority" meant, but the callback signature and invocation pattern didn't support this.

## Fix Summary

The fix introduces a new `get_prio()` callback that allows each scheduler class to extract its priority metric. The `sched_change_begin()` function now captures the old priority value using `get_prio()` before dequeuing the task, then `sched_change_end()` passes this captured value to `prio_changed()`. For deadline tasks, `get_prio_dl()` returns the deadline value, enabling `prio_changed_dl()` to accurately detect deadline changes with `if (p->dl.deadline == old_deadline) return;`.

## Triggering Conditions

This bug manifests when deadline task priorities change and the scheduler needs to decide whether to pull/push tasks between runqueues. The specific conditions are:
- Multiple CPUs with deadline tasks where load balancing decisions matter
- Deadline tasks undergoing priority changes (deadline modifications) via syscalls like `sched_setattr()`
- The deadline scheduler's `prio_changed_dl()` being invoked but receiving only generic `oldprio` instead of the actual old deadline value
- Scenarios where the scheduler would make different decisions if it knew the exact deadline change vs. using the conservative fallback
- Multi-core systems where `!rq->dl.overloaded` condition triggers the fallback `deadline_queue_pull_task(rq)` heuristic

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved for driver). Create deadline tasks on different CPUs and modify their deadlines:
- **Setup**: Use `kstep_topo_init()` and `kstep_topo_apply()` for multi-core topology
- **Task creation**: Create deadline tasks with `kstep_task_create()`, convert to SCHED_DEADLINE policy externally
- **Initial placement**: Pin tasks to specific CPUs using `kstep_task_pin()` to create load imbalance
- **Trigger condition**: Change task deadlines via syscall while monitoring scheduler behavior
- **Observation**: Use `on_sched_balance_selected()` callback to track pull/push decisions
- **Detection**: Log when `prio_changed_dl()` uses fallback vs. accurate deadline comparison
- Compare scheduling decisions before/after deadline changes; bug manifests as suboptimal task placement due to conservative fallback when exact deadline change detection fails
