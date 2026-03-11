# sched/uclamp: Fix rq->uclamp_max not set on first enqueue

- **Commit:** 315c4f884800c45cb6bd8c90422fad554a8b9588
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (uclamp)

## Bug Description

When a task first wakes up on an idle runqueue, the runqueue's `rq->uclamp_max` is not properly reset to match the woken task's `uclamp_max`. This causes tasks with `uclamp_max < 1024` to have their utilization clamping ignored on the first wake-up(s) until the first dequeue to idle after enabling the static key. The bug manifests as tasks running at incorrect CPU frequencies due to improper utilization clamping.

## Root Cause

The bug was introduced when commit d81ae8aac85c changed the default initialization of `rq->uclamp_max` from 0 to 1024. Previously, the code relied on `rq->uclamp_max` being zero, so the comparison in `uclamp_rq_inc_id()` would properly reset it on first enqueue. Additionally, `rq->uclamp_flags` was initialized to 0, which prevented `uclamp_idle_reset()` from being called to update the runqueue's uclamp values. Together, these caused the runqueue's utilization clamping to remain unset on first wake-up.

## Fix Summary

Initialize `rq->uclamp_flags = UCLAMP_FLAG_IDLE` instead of 0 in `init_uclamp_rq()`. This ensures that `uclamp_idle_reset()` is called on the first task enqueue, properly setting the runqueue's `uclamp_max` to match the woken task's utilization clamping value.

## Triggering Conditions

The bug occurs when a task with `uclamp_max < 1024` is the first to wake up on an idle runqueue after system initialization. The triggering sequence requires: (1) An idle CPU runqueue with `rq->uclamp_flags = 0` (buggy initialization), (2) `rq->uclamp_max[UCLAMP_MAX] = 1024` (default), and (3) A task with constrained `uclamp_max < 1024` being woken up as the first task on that runqueue. The bug manifests because `uclamp_rq_inc_id()` fails to reset the runqueue's uclamp_max (since 1024 > task's uclamp_max), and `uclamp_idle_reset()` is never called due to the incorrect flag initialization. This causes the runqueue to operate with an unclamped maximum utilization value instead of the task's constrained value.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (driver on CPU 0, target CPU 1). In `setup()`, create a task with `kstep_task_create()` and constrain its `uclamp_max` using cgroups: `kstep_cgroup_create("test")`, `kstep_cgroup_write("test", "cpu.uclamp.max", "512")`, then `kstep_cgroup_add_task("test", task->pid)`. In `run()`, ensure CPU 1 is idle initially with `kstep_tick_repeat(10)`, then wake the constrained task on CPU 1 with `kstep_task_pin(task, 1, 1)` and `kstep_task_wakeup(task)`. Use `on_tick_begin()` callback to log `cpu_rq(1)->uclamp[UCLAMP_MAX].value` and compare it with the expected task's uclamp_max (512). On buggy kernels, the runqueue will show 1024 instead of 512, indicating the bug is triggered. The fix should properly set the runqueue's uclamp_max to match the task's constraint.
