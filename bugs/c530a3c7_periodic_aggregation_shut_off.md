# sched/psi: Fix periodic aggregation shut off

- **Commit:** c530a3c716b963625e43aa915e0de6b4d1ce8ad9
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

The periodic aggregation worker can ping-pong forever (repeatedly wake itself up), causing infinite work-queue invocations and CPU waste. This occurs when the PSI aggregation worker task itself goes to sleep, as the code no longer includes the check that prevents the worker from waking itself back up.

## Root Cause

Commit 4117cebf1a9f ("psi: Optimize task switch inside shared cgroups") moved task sleep handling from `psi_task_change()` to `psi_task_switch()`. However, the critical check preventing the aggregation worker from waking itself up was not moved along with the code path—it remained in `psi_task_change()` which is no longer called for task sleep transitions. This leaves the aggregation worker unprotected against ping-ponging behavior.

## Fix Summary

Removes the ping-pong prevention check from `psi_task_change()` and moves it into `psi_task_switch()` in the `sleep` branch where task sleep handling now occurs. This ensures the aggregation worker is properly protected against waking itself up infinitely when it goes to sleep.

## Triggering Conditions

The bug requires the PSI aggregation worker (workqueue task with `PF_WQ_WORKER` flag and `psi_avgs_work` as last function) to go to sleep, triggering task switch handling via `psi_task_switch()` instead of `psi_task_change()`. This occurs when:
- PSI accounting is enabled and active
- The periodic aggregation worker is running (`psi_avgs_work` function)
- Worker task transitions to sleep state (TSK_RUNNING cleared)
- Code path goes through `psi_task_switch()` rather than `psi_task_change()`

Without the ping-pong protection check in the sleep branch, the worker wakes itself up infinitely, consuming CPU cycles in an endless work-queue loop.

## Reproduce Strategy (kSTEP)

Configure a system with PSI enabled and trigger workqueue activity to cause the aggregation worker to run and go to sleep:
- Use 2+ CPUs (CPU 0 reserved for driver)
- Create multiple cgroups with `kstep_cgroup_create()` to enable PSI tracking
- Create tasks with `kstep_task_create()` and assign to different cgroups via `kstep_cgroup_add_task()`
- Generate task state changes with `kstep_task_pause()` and `kstep_task_wakeup()` repeatedly
- Use `kstep_tick_repeat()` to advance time and trigger PSI aggregation work
- Monitor for excessive workqueue activity or ping-pong behavior via task switch callbacks
- Detect bug by observing the PSI worker task (`psi_avgs_work`) repeatedly waking itself without making progress
