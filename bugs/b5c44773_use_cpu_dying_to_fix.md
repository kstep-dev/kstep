# sched: Use cpu_dying() to fix balance_push vs hotplug-rollback

- **Commit:** b5c4477366fb5e6a2f0f38742c33acd666c07698
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

When CPU hotplug operations fail and roll back partway through the state machine, the balance_push mechanism fails to terminate or activate at the correct time. Specifically, if cpu_down() fails after sched_cpu_deactivate() but before the rollback reaches sched_cpu_activate(), balance_push remains active when it should terminate. Conversely, if cpu_up() fails and reverts to offline, balance_push should be active but is not. This leaves the system in an inconsistent state where task migration behavior is incorrect during failed hotplug operations.

## Root Cause

The previous implementation relied on a simple boolean flag `rq->balance_push` that was managed by CPU hotplug notifiers (sched_cpu_dying/sched_cpu_activate). However, these notifiers only fire at specific points in the hotplug state machine. If the operation fails and rolls back at intermediate points, the notifier sequence becomes incorrect, causing balance_push to be in the wrong state. The flag does not reflect the actual CPU state during rollback scenarios.

## Fix Summary

Instead of using a separate boolean flag managed by notifiers, the fix gates balance_push's utility directly with the `cpu_dying()` state, which accurately reflects the CPU's actual hotplug status. The `balance_push()` function now checks `cpu_dying()` to determine if it should be active, removing the disconnect between the notifier-driven flag and the actual system state. This ensures balance_push activates correctly for task migration during all CPU offline paths and deactivates properly during online paths, regardless of rollback scenarios.

## Triggering Conditions

This bug requires a CPU hotplug operation that fails during the state machine transition, specifically when:
- CPU offline fails after `sched_cpu_deactivate()` but before `sched_cpu_activate()` during rollback, leaving `balance_push` active when it should terminate
- CPU online fails and reverts to offline state, where `balance_push` should be active but remains inactive
- The scheduler's `is_cpu_allowed()` function incorrectly checks the stale `rq->balance_push` flag instead of the actual CPU hotplug state
- Kernel threads (particularly non-per-CPU kthreads) attempt to schedule on CPUs in inconsistent hotplug states
- The race occurs when the notifier-based flag management becomes out of sync with the actual CPU state transitions

## Reproduce Strategy (kSTEP)

This bug is difficult to reproduce with kSTEP as it requires actual CPU hotplug failure scenarios that are not easily simulated in the framework. However, a potential approach would be:
- Use at least 2 CPUs (CPU 0 reserved for driver, test on CPU 1+)
- Create kernel threads with `kstep_kthread_create()` and bind them using `kstep_kthread_bind()` to specific CPUs
- In `setup()`, create multiple kernel threads pinned to the target CPU
- Simulate the inconsistent state by directly manipulating the `rq->balance_push` flag (if accessible) or hooking into hotplug notifiers
- Use `on_tick_begin()` callback to monitor task scheduling behavior and check `is_cpu_allowed()` return values
- Log whether kernel threads can be scheduled on CPUs that should be in dying state but have incorrect balance_push settings
- Detection: Verify that kernel threads are incorrectly allowed/disallowed based on mismatched balance_push vs actual CPU state
