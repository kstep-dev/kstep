# sched: Fix faulty assertion in sched_change_end()

- **Commit:** 1862d8e264def8425d682f1177e22f9fe7d947ea
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

An overly strict assertion in `sched_change_end()` was triggering when `rt_mutex_setprio()` demoted a task to a lower priority class. The assertion assumed that any class demotion would have `need_resched` already set, but this wasn't guaranteed. The assertion was also incorrectly applied to all tasks rather than only running tasks.

## Root Cause

The assertion added in commit 47efe2ddccb1f made an incorrect assumption: that the caller would always set `need_resched` when demoting a task's scheduling class. However, `rt_mutex_setprio()` does not force a reschedule on class demotion, causing the assertion to fail in practice. Additionally, the assertion applied to non-running tasks where rescheduling isn't immediately relevant.

## Fix Summary

Replace the assertion with proactive code that explicitly calls `resched_curr(rq)` when a running task is demoted to a lower priority class. This ensures the scheduler properly handles task demotion and only applies this logic to running tasks where it matters.

## Triggering Conditions

The bug manifests when `rt_mutex_setprio()` demotes a **running** task from a higher scheduling class to a lower one (e.g., RT to CFS) without setting `need_resched`. This occurs in the `sched_change_end()` code path during class transitions with `ENQUEUE_CLASS` flag set. The faulty assertion expects that any class demotion should already have `need_resched` set, but `rt_mutex_setprio()` does not guarantee this. The assertion applies to all tasks (not just running ones), making it overly broad. The race happens when the task is currently running (`ctx->running` is true) and experiences a scheduling class demotion.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs. In `setup()`, create an RT task and a CFS task. Use `kstep_task_fifo(rt_task)` to make one task RT priority. In `run()`, start both tasks with `kstep_task_wakeup()` and let the RT task run via `kstep_tick_repeat(5)`. Simulate `rt_mutex_setprio()` demotion by calling `kstep_task_cfs(rt_task)` while the RT task is running on its CPU. The buggy kernel should trigger the assertion failure in `sched_change_end()`. Use `on_tick_begin()` callback to log current running tasks and their classes. Check kernel logs for the `WARN_ON_ONCE` assertion failure message. The fixed kernel should handle the demotion without assertion failure and properly call `resched_curr()`.
