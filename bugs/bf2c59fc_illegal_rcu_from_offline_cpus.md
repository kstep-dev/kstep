# sched/core: Fix illegal RCU from offline CPUs

- **Commit:** bf2c59fce4074e55d622089b34be3a6bc95484fb
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

During CPU hotplug offline, mmdrop() was being called from the idle task after RCU had already stopped monitoring the offline CPU. This caused lockdep warnings when mmdrop() attempted to use RCU through memcg or debugobjects. The warning message shows: "RCU used illegally from offline CPU!" This violates RCU's contract and triggers lockdep complaints.

## Root Cause

The bug occurs because idle_task_exit() explicitly called mmdrop(mm) on the dying CPU, but by that point in the offline sequence, after rcu_report_dead() has been called, RCU is no longer monitoring that CPU. When mmdrop() tries to use RCU-protected resources, lockdep detects the illegal RCU usage from an offline CPU.

## Fix Summary

The fix defers the active_mm cleanup from the dying CPU to the boot processor by removing the mmdrop() call and the explicit active_mm assignment from idle_task_exit(). Instead, finish_cpu() running on the boot processor is relied upon to clean up the idle task's memory management state. This moves the RCU-dependent cleanup to when RCU is still active.

## Triggering Conditions

The bug requires a specific sequence during CPU hotplug offline: (1) A CPU must be going offline with the idle task having an active_mm that differs from init_mm, (2) rcu_report_dead() must have been called, stopping RCU monitoring of the dying CPU, (3) idle_task_exit() then executes on the dying CPU and calls mmdrop(), (4) mmdrop() attempts to use RCU-protected resources (via memcg or debugobjects), triggering the "RCU used illegally from offline CPU!" warning. The race occurs because the memory management cleanup happens after RCU protection is withdrawn, creating a window where RCU-dependent code runs on an unmonitored CPU.

## Reproduce Strategy (kSTEP)

Reproducing this bug with kSTEP is challenging since it requires actual CPU hotplug operations which are not directly exposed in the kSTEP framework. However, a potential approach would be: (1) Use at least 2 CPUs (CPU 0 reserved for driver), (2) In setup(), create tasks on CPU 1 with non-init_mm active_mm states using kstep_task_create() and memory operations, (3) In run(), simulate conditions where CPU 1's idle task would have active_mm != init_mm, (4) Use callbacks like on_tick_end() to monitor RCU state and detect illegal usage patterns, (5) Look for lockdep warnings in kernel logs via TRACE_INFO() when RCU-protected cleanup occurs. Since direct CPU offline simulation may not be feasible, focus on detecting the RCU usage violation patterns through kernel logging and state inspection.
