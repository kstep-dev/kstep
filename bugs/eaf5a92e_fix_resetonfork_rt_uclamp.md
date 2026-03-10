# sched/core: Fix reset-on-fork from RT with uclamp

- **Commit:** eaf5a92ebde5bca3bb2565616115bd6d579486cd
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When a task with the reset-on-fork flag is forked from a real-time (RT) task, the `uclamp_fork()` function incorrectly applies RT-specific uclamp settings to the child task. Specifically, it sets `uclamp.min` to 1024 (100% boost) for the child task because the parent is an RT task. However, immediately after this, the child task's scheduling policy is lowered to SCHED_NORMAL, causing the child task to inherit the wrong uclamp minimum value.

## Root Cause

The bug occurs in `uclamp_fork()` which checks `rt_task(p)` to apply special RT task handling when the reset-on-fork flag is set. This check is incorrect because it doesn't account for the fact that the task's policy is changed from RT to SCHED_NORMAL immediately after `uclamp_fork()` returns. This creates a race condition or ordering issue where the uclamp value is set based on the old RT policy, but the policy has already been (or is about to be) changed to SCHED_NORMAL.

## Fix Summary

The fix removes the special RT task handling from `uclamp_fork()` when reset-on-fork is set. Instead of checking if the task is an RT task and applying 100% boost, it simply resets all uclamp values to their defaults using `uclamp_none(clamp_id)`. This is correct because when reset-on-fork is active, the child task should receive default uclamp values regardless of the parent task's policy, since the policy itself is being reset.

## Triggering Conditions

The bug is triggered when an RT task (SCHED_FIFO or SCHED_RR) with the reset-on-fork flag set forks a child process. During fork, `uclamp_fork()` incorrectly checks `rt_task(p)` on the child task, which still has the parent's RT policy at that moment, and sets `uclamp.min` to 1024 (100% boost). However, immediately after `uclamp_fork()` returns, the reset-on-fork logic changes the child's policy to SCHED_NORMAL. This creates an inconsistent state where a SCHED_NORMAL task has RT-level uclamp settings. The bug requires: 1) an RT parent task, 2) the reset-on-fork flag being set, and 3) any fork operation that creates a child process.

## Reproduce Strategy (kSTEP)

Create an RT parent task with reset-on-fork and verify child's incorrect uclamp inheritance. Use 2+ CPUs (CPU 0 reserved). In `setup()`: Create RT parent with `kstep_task_create()` + `kstep_task_fifo()`, then set reset-on-fork flag via task struct manipulation. In `run()`: Use `kstep_task_fork()` to create child process, triggering the uclamp_fork() bug. In callbacks: Use `on_tick_end()` to monitor both parent and child task uclamp values via task struct inspection. Check if child has `p->uclamp_req[UCLAMP_MIN].value == 1024` (buggy) vs default value (fixed). Also verify child's policy changed to SCHED_NORMAL while retaining wrong uclamp values. Log uclamp settings and task policies to detect the inconsistency.
