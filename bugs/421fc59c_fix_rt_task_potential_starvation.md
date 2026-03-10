# sched/deadline: Fix RT task potential starvation when expiry time passed

- **Commit:** 421fc59cf58c64f898cafbbbbda0bc705837e7df
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

RT tasks on the runqueue may not be scheduled as expected when the fair server mechanism is in use. The issue manifests when the deadline server's timer expiration time is already in the past, causing the timer registration to fail repeatedly. This results in the DL entity being re-enqueued without proper state management, potentially starving RT tasks from CPU time.

## Root Cause

When `start_dl_timer()` fails (because the expiration time is already in the past), the code immediately re-enqueues the DL entity with `ENQUEUE_REPLENISH` flag without first calling `replenish_dl_new_period()`. This causes the timer to continue using the stale deadline, leading to repeated timer registration failures and the DL entity being stuck in a cycle of enqueueing without proper replenishment.

## Fix Summary

The fix ensures that when timer registration fails for a deadline server, the DL entity is first properly replenished with `replenish_dl_new_period()` to update its deadline, and then `start_dl_timer()` is retried with the new deadline. This prevents the DL entity from being stuck with an expired deadline and allows RT tasks to be scheduled as expected.

## Triggering Conditions

This bug occurs in the deadline scheduler's throttle path when:
- The fair server deadline mechanism is active (deadline server managing fair tasks)  
- A DL entity (specifically a dl_server) exceeds its runtime and enters the throttle path in `update_curr_dl_se()`
- The DL server's timer deadline has already passed (expires field is in the past)
- `start_dl_timer()` fails because the timer expiration time is stale
- The system repeatedly re-enqueues the DL entity without updating its deadline
- RT tasks on the runqueue become starved because the DL server's timer never gets properly restarted

## Reproduce Strategy (kSTEP)

Setup at least 2 CPUs (CPU 0 reserved for driver). Create RT tasks using `kstep_task_create()` + `kstep_task_fifo()` and pin them with `kstep_task_pin()`. Create heavy fair load using multiple CFS tasks to activate the fair server mechanism. Use `kstep_tick_repeat()` to advance time and force fair server runtime exhaustion. Monitor the system with `on_tick_begin` callback to track RT task scheduling and fair server state. Force the DL server timer deadline into the past by creating timing pressure through aggressive ticking. Check if RT tasks get scheduled (via `kstep_output_curr_task()` in callbacks). The bug manifests as RT task starvation when the DL server timer fails to restart properly, observable through lack of RT task execution despite being runnable.
