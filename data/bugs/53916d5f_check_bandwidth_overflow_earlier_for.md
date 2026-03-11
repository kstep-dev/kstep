# sched/deadline: Check bandwidth overflow earlier for hotplug

- **Commit:** 53916d5fd3c0b658de3463439dd2b7ce765072cb
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

During CPU hotplug (deactivation), there is a race window between when a CPU is removed from scheduling and active_mask and when deadline bandwidth overflow checks occur. A throttled DEADLINE task can have its replenishment timer fire while its CPU is being considered offline, but before the kernel can reject the hotplug operation due to bandwidth overflow. This creates a window where the task sees stale scheduler state and the system cannot properly roll back the operation.

## Root Cause

The deadline bandwidth overflow check (dl_bw_deactivate()) was called late in sched_cpu_deactivate(), after the CPU had already been removed from various scheduler structures. The check needed to compute bandwidth with the CPU still considered online, but it was happening after offline state was already set, creating a race where replenishment timers could fire during this inconsistent window.

## Fix Summary

The fix moves dl_bw_deactivate() to the very beginning of sched_cpu_deactivate(), before any CPU state changes. The deadline bandwidth calculation is adjusted to explicitly consider the CPU as offline when checking for overflow. This eliminates the race window and simplifies the code by removing rollback logic that is no longer needed.

## Triggering Conditions

The bug requires a specific race between deadline task throttling/replenishment and CPU hotplug:
- Multiple CPUs with DEADLINE tasks consuming significant bandwidth (approaching limits)
- A throttled DEADLINE task with an active replenishment timer
- CPU hotplug deactivation initiated while the task's timer is about to fire
- The replenishment timer fires after sched_cpu_deactivate() removes the CPU from active_mask but before dl_bw_deactivate() checks bandwidth overflow
- This creates a window where the task sees inconsistent scheduler state (CPU offline but timer still firing)
- The bandwidth overflow check fails to account for the timer firing during this inconsistent state

## Reproduce Strategy (kSTEP)

Configure a multi-CPU system with deadline bandwidth pressure and trigger hotplug during replenishment:
- Set up 3+ CPUs (CPU 0 reserved for driver, use CPUs 1-3)
- Create multiple DEADLINE tasks with high bandwidth requirements that approach system limits
- Use `kstep_task_create()` and configure with deadline scheduling parameters via sysfs writes
- Fill CPUs 1-3 with deadline tasks, causing some to throttle when bandwidth is exceeded
- Use `kstep_tick_repeat()` to advance time and trigger task throttling
- Monitor task state via `on_tick_begin()` callback to detect when a task becomes throttled
- Precisely time CPU hotplug simulation during the replenishment window using `kstep_cpu_set_capacity(cpu, 0)` to effectively offline a CPU
- Detect the race by logging inconsistent state: replenishment timers firing while CPU appears offline in scheduler structures
- Check for bandwidth calculation errors and failed rollback scenarios in the logs
