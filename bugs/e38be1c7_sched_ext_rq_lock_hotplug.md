# sched_ext: Fix rq lock state in hotplug ops

- **Commit:** e38be1c7647c8c78304ce6d931b3b654e27948b3
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

During CPU hotplug operations (bringing CPUs online or offline), the `ops.cpu_online()` and `ops.cpu_offline()` callbacks incorrectly convey that the rq is locked when it is not actually locked, triggering a WARNING in kernel/sched/sched.h:1504. This indicates a lock state tracking violation that could mask other synchronization issues.

## Root Cause

The `handle_hotplug()` function was calling the hotplug callbacks with `SCX_KF_UNLOCKED` flag but passing the `rq` pointer as an argument. The underlying rq lock tracking logic (introduced in commit 18853ba782bef) interprets the presence of a non-NULL rq pointer as indicating the rq is locked, creating an inconsistency: the flag says unlocked but the argument says locked. This mismatch triggers an assertion warning that verifies lock state consistency.

## Fix Summary

The fix changes the hotplug callback invocations to pass `NULL` instead of the `rq` pointer, preventing the rq from being tracked as locked during `ops.cpu_online()` and `ops.cpu_offline()` calls. This aligns the actual lock state (unlocked) with what is communicated to the lock tracking logic.

## Triggering Conditions

This bug occurs during CPU hotplug operations when sched_ext is enabled. The precise conditions are:
- sched_ext scheduler must be active (enabled via `scx_ops`)
- A CPU hotplug event occurs (CPU going online or offline via `/sys/devices/system/cpu/cpuX/online`)
- The scheduler implements `ops.cpu_online()` or `ops.cpu_offline()` callbacks
- The `handle_hotplug()` function calls these callbacks with `SCX_KF_UNLOCKED` flag but non-NULL rq pointer
- The rq lock tracking logic (from commit 18853ba782bef) detects the inconsistency between the flag (unlocked) and argument (suggesting locked)
- This triggers the WARNING at kernel/sched/sched.h:1504 during lock state verification

## Reproduce Strategy (kSTEP)

To reproduce this bug using kSTEP framework:
- Configure QEMU with at least 3 CPUs (CPU 0 reserved for driver, need CPU 1+ for hotplug)
- In `setup()`: Enable sched_ext by loading a scheduler that implements cpu_online/cpu_offline callbacks
- In `run()`: Use `kstep_write("/sys/devices/system/cpu/cpu2/online", "0", 1)` to offline CPU 2
- Follow with `kstep_write("/sys/devices/system/cpu/cpu2/online", "1", 1)` to bring it back online
- Use `on_tick_begin` callback to monitor kernel logs for the WARNING at sched.h:1504
- Check for warning messages containing "handle_hotplug" in dmesg output
- The bug is triggered if the WARNING appears during hotplug operations
- Test with multiple online/offline cycles to ensure consistent reproduction
- On the buggy kernel, the WARNING should fire; on the fixed kernel, no warning should appear
