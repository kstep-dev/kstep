# sched: Prevent balance_push() on remote runqueues

- **Commit:** 868ad33bfa3bf39960982682ad3a0f8ebda1656e
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (run-queue balance callback)

## Bug Description

The `balance_push()` function can be invoked on remote run-queues when called from `sched_setscheduler()` and `rt_mutex_setprio()` after changing task priorities or scheduling class. However, `balance_push()` is only valid to be invoked on the outgoing CPU during CPU hotplug. When called on a remote CPU, it triggers a debug warning and leaves the per-CPU variable `push_work` unprotected, resulting in double enqueues on the stop machine list.

## Root Cause

The original code used only a `SCHED_WARN_ON()` check that triggered when called remotely, but this warning was insufficient to prevent the problem. The function lacked proper validation to detect remote invocations and return early. Without this validation, the per-CPU `push_work` variable can be enqueued multiple times on the stop machine list when multiple CPUs invoke `balance_push()` concurrently.

## Fix Summary

The fix removes the WARN_ON check and adds a proper validation condition `rq != this_rq()` that causes the function to return early if invoked on a remote run-queue. This ensures `balance_push()` only executes its critical section when called on the outgoing CPU, protecting the per-CPU `push_work` variable from concurrent access.

## Triggering Conditions

The bug requires a CPU hotplug scenario where `balance_push()` is invoked on remote run-queues. This occurs when:
- One CPU is in the dying state (`cpu_dying()` returns true)
- Multiple tasks have their priorities or scheduling classes changed via `sched_setscheduler()` or `rt_mutex_setprio()`
- These functions invoke run-queue balance callbacks that can target remote CPUs
- Without the `rq != this_rq()` check, `balance_push()` executes on the wrong CPU
- The per-CPU `push_work` variable gets accessed unsafely, causing potential double enqueues on the stop machine list
- Race condition occurs when multiple CPUs concurrently invoke `balance_push()` for the same dying CPU

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce with kSTEP as it involves CPU hotplug simulation. A potential approach:
- Use 3+ CPUs (CPU 0 reserved for driver, need 2+ for multi-CPU scenario)
- Create multiple tasks pinned to different CPUs using `kstep_task_create()` and `kstep_task_pin()`
- Simulate the dying CPU state by manually setting relevant scheduler state (may require direct kernel manipulation)
- Trigger priority changes using `kstep_task_set_prio()` on multiple tasks from different CPUs concurrently
- Use `on_tick_begin()` callback to monitor and log `balance_push()` invocations and per-CPU `push_work` state
- Detect the bug by observing the SCHED_WARN_ON trigger (in buggy kernel) or checking for double enqueues
- Log when `balance_push()` is called with `rq->cpu != smp_processor_id()` to confirm remote invocation
- Verify the fix prevents remote execution by confirming early return when `rq != this_rq()`
