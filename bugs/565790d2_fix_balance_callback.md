# sched: Fix balance_callback()

- **Commit:** 565790d28b1e33ee2f77bad5348b99f6dfc366fd
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

The balance_callback() function was called after rq->lock was dropped, creating a race condition where another CPU could interleave and access/modify the callback list concurrently. This violated the design intent that balance operations should be delayed until the end of the current rq->lock section to ensure safe execution. The race condition could lead to incorrect callback execution, skipped balance operations, or memory safety issues.

## Root Cause

The original code executed balance_callback() after releasing rq->lock in multiple code paths (schedule_tail, __schedule when not switching, rt_mutex_setprio). Since balance operations often need to drop rq->lock temporarily, the lock release window before balance_callback() was invoked allowed other CPUs to interleave and touch the balance callback list, breaking the mutual exclusion guarantee that was needed.

## Fix Summary

The fix restructures the balance callback mechanism to ensure callbacks are invoked while still holding rq->lock. It introduces helper functions that either execute callbacks before unlocking (do_balance_callbacks), splice the callback list onto a local stack (splice_balance_callbacks), or wrap late callback invocations to re-acquire the lock. This guarantees the balance list is always empty when rq->lock is taken and only the current CPU executes its own callbacks.

## Triggering Conditions

The race occurs during any code path where balance callbacks are queued and rq->lock is dropped before executing them. Critical paths include: schedule_tail() after task switch completion, __schedule() when no context switch occurs, and rt_mutex_setprio() after priority changes. The race window requires: (1) CPU A queues balance callbacks on its rq->balance_callback list, (2) CPU A drops rq->lock before calling balance_callback(), (3) CPU B acquires the same rq->lock and modifies the callback list (adding/removing callbacks), (4) CPU A resumes and executes stale/corrupted callbacks. SMP systems with active load balancing, RT priority inheritance, or frequent task migrations are most vulnerable.

## Reproduce Strategy (kSTEP)

Use 3+ CPUs (CPU 0 reserved for driver). Create RT and CFS tasks to trigger priority inheritance scenarios that queue balance callbacks. Use kstep_task_create() to create multiple tasks, kstep_task_fifo() for RT tasks, and kstep_task_pin() to control CPU placement. In setup(), create tasks on different CPUs and establish RT priority inheritance chains. In run(), trigger priority changes via kstep_task_set_prio() while simultaneously causing load balance operations with kstep_task_wakeup(). Use on_tick_begin() callback to inject controlled timing delays and kstep_tick_repeat() to advance time. Monitor rq->balance_callback list state via logging in custom kernel probes. The race manifests as callback list corruption, missing balance operations, or kernel crashes during callback execution.
