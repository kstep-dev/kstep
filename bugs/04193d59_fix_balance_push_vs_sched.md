# sched: Fix balance_push() vs __sched_setscheduler()

- **Commit:** 04193d590b390ec7a0592630f46d559ec6564ba1
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

During CPU hotplug operations, the balance_push_callback must remain on the callback list at all times to filter out task selection for the CPU being taken down. However, __sched_setscheduler() calls splice_balance_callbacks() which removes all callbacks from the list across a lock-break period. This creates a window where __schedule() can interleave and observe an empty callback list, bypassing the balance_push() filter and allowing tasks to be incorrectly scheduled on a CPU that is going down.

## Root Cause

The splice_balance_callbacks() function unconditionally removes all callbacks from rq->balance_callback, including balance_push_callback, before releasing and re-acquiring the rq->lock. During this lock-break, __schedule() may run and check rq->balance_callback, finding it empty instead of the expected balance_push_callback, thus skipping the essential filtering logic needed during CPU hotplug.

## Fix Summary

The fix introduces a split parameter to the callback splicing logic: __splice_balance_callbacks(rq, split). When split=true (indicating a lock-break period), it preserves balance_push_callback on the list by returning NULL instead of removing it. When split=false (in contexts without lock breaks), it removes all callbacks as before. This ensures the balance_push filter remains active during the entire CPU hotplug sequence.

## Triggering Conditions

The bug occurs when CPU hotplug is in progress (balance_push_callback is active on rq->balance_callback) and __sched_setscheduler() is called concurrently on the same runqueue. This requires:
- A CPU undergoing hotplug offline operation, with balance_push_callback installed
- The RT task scheduler path via __sched_setscheduler() calling splice_balance_callbacks() 
- A scheduling context break where rq->lock is released and re-acquired
- An interleaving __schedule() call that observes the temporarily empty balance_callback list
- The runqueue must have tasks that would normally be rejected by balance_push() filter
The race window occurs between splice_balance_callbacks() removing all callbacks and the subsequent balance_callbacks() restoring them.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 1 for hotplug target, CPU 0 reserved for driver). Setup simulated CPU hotplug state by manually installing balance_push_callback on CPU 1's runqueue. Create RT and CFS tasks pinned to CPU 1. In run(), use kstep_task_set_prio() to trigger __sched_setscheduler() on an RT task while balance_push_callback is active. Monitor rq->balance_callback state during the operation using on_tick_begin() callback or kernel printk. The bug manifests when balance_push_callback temporarily disappears from the list, allowing tasks to remain scheduled on the "hotplugged" CPU. Use kstep_tick_repeat() to advance scheduling and capture the race condition. Detection involves checking if tasks remain on CPU 1 when they should have been migrated by balance_push().
