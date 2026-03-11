# sched: Fix the do_set_cpus_allowed() locking fix

- **Commit:** af13e5e437dc2eb8a3291aad70fc80d9cc78bc73
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The previous locking fix (abfc01077df6) introduced a deadlock in `__balance_push_cpu_stop()`. When `select_fallback_rq()` is called while holding `rq->lock`, it internally triggers code paths that attempt to recursively acquire `rq->lock` (via `set_cpus_allowed_force()`), causing the machine to lock up. This deadlock occurs during CPU hotplug operations on systems with CPU affinity changes.

## Root Cause

The bug exists because `select_fallback_rq()` was called inside the critical section while `rq->lock` was held. The function indirectly invokes `set_cpus_allowed_force()`, which attempts to acquire `rq->lock` again. Since spin locks on a single CPU cannot be held recursively, this causes a deadlock. The previous fix overlooked the lock dependency chain introduced by this call sequence.

## Fix Summary

The fix moves `select_fallback_rq()` execution outside of the `rq->lock` critical section, calling it while only holding `p->pi_lock`. This prevents the recursive lock attempt while maintaining synchronization through `p->pi_lock`, which fully serializes access and prevents race conditions from the earlier CPU acquisition of the fallback CPU being invalidated.

## Triggering Conditions

The deadlock occurs specifically during CPU hotplug operations in `__balance_push_cpu_stop()`, which is called as a stop_machine callback to migrate tasks off an outgoing CPU. The bug requires:
- A task that needs migration due to CPU hotplug (its current CPU going offline)
- The task's cpumask needs to be invalid for the current CPU, forcing `select_fallback_rq()` to call `set_cpus_allowed_force()`  
- The `__balance_push_cpu_stop()` function executing with both `p->pi_lock` and `rq->lock` held
- `select_fallback_rq()` internally invoking `set_cpus_allowed_force()` which tries to recursively acquire the same `rq->lock`
- This creates an immediate deadlock since the same CPU cannot acquire the same spinlock recursively

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce in kSTEP since it requires actual CPU hotplug operations, which kSTEP doesn't directly support. However, we can simulate the locking scenario:
- Use 2+ CPUs (CPU 0 reserved for driver)
- Create a task with `kstep_task_create()` and pin it to CPU 1 with `kstep_task_pin(task, 1, 1)`
- Manually invoke the problematic code path by accessing kernel internals to simulate `__balance_push_cpu_stop()` execution
- Set up a custom callback using `on_tick_begin` to monitor for deadlock symptoms (hung system, no progress)
- Artificially restrict the task's cpumask to force `select_fallback_rq()` to call `set_cpus_allowed_force()`
- Check for system responsiveness - successful reproduction would show the system hanging during migration
- Compare behavior before/after the fix by testing on buggy vs fixed kernel versions
