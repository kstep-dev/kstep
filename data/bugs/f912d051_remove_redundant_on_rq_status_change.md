# sched: remove redundant on_rq status change

- **Commit:** f912d051619d11411867f642d2004928eb0b41b1
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The code in `try_steal_cookie()` was manually setting the `on_rq` status of a task both before calling `deactivate_task()` and after calling `activate_task()`. This created redundant state changes that interfered with the proper internal state management performed by these functions. The redundancy could leave the task's `on_rq` state inconsistent with the scheduler's internal tracking, potentially causing scheduling anomalies or incorrect task migration behavior.

## Root Cause

The `deactivate_task()` and `activate_task()` functions are designed to manage the task's `on_rq` status as part of their internal operations. The manual assignments to `p->on_rq = TASK_ON_RQ_MIGRATING` before `deactivate_task()` and `p->on_rq = TASK_ON_RQ_QUEUED` after `activate_task()` override the state changes made by these functions, resulting in redundant and potentially conflicting state transitions. This violates the principle of encapsulation and can cause the task's state representation to become inconsistent.

## Fix Summary

The fix removes the two manual `on_rq` state assignments, allowing `deactivate_task()` and `activate_task()` to manage the task state correctly without interference. This ensures the task's `on_rq` status remains consistent with the scheduler's internal state throughout the migration process.

## Triggering Conditions

This bug occurs in the core scheduling subsystem's `try_steal_cookie()` function during task migration. The precise conditions needed are: (1) Core scheduling must be enabled with tasks having matching security cookies, (2) One CPU must be idle while another CPU has runnable tasks with the same cookie, (3) The cookie stealing mechanism attempts to migrate a task from the busy CPU to the idle CPU, (4) During this migration, the redundant `on_rq` status assignments override the internal state management of `deactivate_task()` and `activate_task()`, potentially leaving the task's `on_rq` status inconsistent with the scheduler's internal tracking.

## Reproduce Strategy (kSTEP)

Use at least 3 CPUs (CPU 0 reserved for driver). In `setup()`, enable core scheduling via sysctl and create 2-3 tasks with `kstep_task_create()`. In `run()`, assign tasks to the same cgroup to give them matching cookies using `kstep_cgroup_create()` and `kstep_cgroup_add_task()`. Pin 2 tasks to CPU 1 using `kstep_task_pin()` to make it busy, and ensure CPU 2 stays idle. Use `kstep_tick_repeat()` to let the system reach steady state. Force load balancing by enabling the periodic balancer or manually triggering balance via `kstep_tick_repeat()` with load balancing enabled. Monitor the task's `on_rq` status during migration using `on_sched_softirq_begin()` and `on_sched_softirq_end()` callbacks to detect inconsistent state transitions. The bug is triggered when cookie stealing occurs and the redundant assignments interfere with proper state management.
