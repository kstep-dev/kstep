# sched/core: Fix psi_dequeue() for Proxy Execution

- **Commit:** c2ae8b0df2d1bb7a063f9e356e4e9a06cd4afe11
- **Affected file(s):** kernel/sched/stats.h
- **Subsystem:** core

## Bug Description

When Proxy Execution mutex-blocked tasks are dequeued after being switched off the CPU, the `psi_dequeue()` function incorrectly skips clearing PSI state flags. This causes the task to remain marked as `TSK_RUNNING` when it should not be, leading to a "psi: inconsistent task state!" kernel error when the task is later re-enqueued and `TSK_RUNNING` is set again on an already-running task.

## Root Cause

The original `psi_dequeue()` function assumed that a `DEQUEUE_SLEEP` flag always indicates a voluntary sleep followed immediately by a `psi_task_switch()` call, which would handle clearing `TSK_RUNNING` and other flags. However, Proxy Execution violates this assumption: mutex-blocked tasks can be dequeued without an immediately following task switch, leaving `TSK_RUNNING` set while `TSK_ONCPU` has already been cleared elsewhere. The early return on `DEQUEUE_SLEEP` then prevents the necessary flag cleanup.

## Fix Summary

The fix extends the `DEQUEUE_SLEEP` check to verify that `TSK_ONCPU` is still set before returning early. If `TSK_ONCPU` is not set, the task has already been switched away and the dequeue operation must clear the remaining PSI flags as usual, rather than deferring to a task switch that is no longer imminent.

## Triggering Conditions

The bug requires Proxy Execution to be enabled and a mutex-blocked task to experience a specific sequence: the task must be switched off CPU (clearing `TSK_ONCPU`) while remaining on the runqueue with `TSK_RUNNING` set, then subsequently dequeued with `DEQUEUE_SLEEP` when `find_proxy_task()` cannot handle the case (e.g., mutex owner on remote CPU or sleeping). The dequeue must not be immediately followed by `psi_task_switch()`. When the task is later re-enqueued, `psi_enqueue()` attempts to set `TSK_RUNNING` again on an already-running task, triggering the "psi: inconsistent task state!" error.

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs. In `setup()`: enable Proxy Execution via kernel config, create two tasks and a shared mutex. In `run()`: have task A acquire mutex and run on CPU 1, then task B block on the same mutex (triggering proxy execution). Use `kstep_task_pause(task_a)` to make the mutex owner unavailable, forcing task B to be dequeued while retaining `TSK_RUNNING` status. Call `kstep_task_wakeup(task_b)` to re-enqueue task B. Use `on_tick_begin` callback to monitor PSI flags via task->psi_flags and watch for kernel error messages. The bug manifests as "psi: inconsistent task state!" in kernel logs when `psi_enqueue()` tries to set `TSK_RUNNING` on a task already marked as running.
