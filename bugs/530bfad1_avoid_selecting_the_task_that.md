# sched/core: Avoid selecting the task that is throttled to run when core-sched enable

- **Commit:** 530bfad1d53d103f98cec66a3e491a36d397884d
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/fair.c, kernel/sched/rt.c, kernel/sched/sched.h
- **Subsystem:** core (core-scheduling with throttling)

## Bug Description

When core-scheduling is enabled and a task's runqueue (rt_rq, cfs_rq) or deadline task is throttled, the task may still be present in the core scheduling tree because throttled tasks are not dequeued from it. The `sched_core_find()` and `sched_core_next()` functions may therefore return throttled tasks, causing them to be incorrectly scheduled to run on the CPU despite being throttled.

## Root Cause

The core scheduling tree is not automatically updated when a task becomes throttled. The functions that search the tree to find eligible tasks matching a given core cookie do not perform throttling checks, allowing throttled tasks to be selected for execution.

## Fix Summary

The fix introduces a `sched_task_is_throttled()` helper that checks if a task is throttled by delegating to scheduler-class-specific callbacks. The `sched_core_find()` and `sched_core_next()` functions are modified to skip throttled tasks, ensuring only runnable and unthrottled tasks are returned. Additionally, scheduler classes (RT, Fair, Deadline) implement the `task_is_throttled` callback, and an extra check is added in `try_steal_cookie()` to prevent stealing to a throttled destination runqueue.

## Triggering Conditions

This bug requires core-scheduling to be enabled (CONFIG_SCHED_CORE=y) and tasks with assigned core cookies. The triggering conditions are:
- Core-scheduling is active with cookied tasks present in the core tree
- At least one task's runqueue becomes throttled (RT bandwidth exceeded, CFS group throttled, or DL task throttled) 
- The throttled task remains in the core tree because core-sched doesn't dequeue throttled tasks
- During core scheduling decisions, `sched_core_find()` or `sched_core_next()` traverses the tree looking for tasks matching a cookie
- The functions return the throttled task without checking its throttling status
- The core scheduler attempts to run the throttled task, violating bandwidth/throttling constraints

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved for driver, need sibling cores for core-sched). In `setup()`:
- Enable core-scheduling via sysctl and create cgroups with bandwidth limits 
- Create RT/CFS tasks with assigned core cookies and bandwidth constraints that will cause throttling
- Pin tasks to sibling CPUs to trigger core scheduling decisions

In `run()`:
- Use `kstep_task_wakeup()` to start tasks that will consume their bandwidth quota quickly
- Use `kstep_tick_repeat()` to advance time until bandwidth is exhausted and runqueues become throttled 
- Continue ticking while tasks are throttled but still present in core tree
- Use `on_tick_begin()` callback to log when throttled tasks are incorrectly selected by core scheduler
- Check kernel logs for evidence of throttled task execution or use task runtime accounting to detect violations
