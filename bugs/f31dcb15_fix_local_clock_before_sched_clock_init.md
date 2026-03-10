# sched/clock: Fix local_clock() before sched_clock_init()

- **Commit:** f31dcb152a3d0816e2f1deab4e64572336da197d
- **Affected file(s):** kernel/sched/clock.c
- **Subsystem:** clock

## Bug Description

Before sched_clock_init() has run, local_clock() incorrectly returns stale or incorrect clock values instead of falling back to sched_clock(). This causes kernel timestamps in early boot to get stuck at a fixed value (e.g., 0.001000) and remain unchanged until sched_clock_init() completes, breaking the progression of dmesg timestamps.

## Root Cause

When local_clock() was refactored into a noinstr implementation (commit 776f22913b8e), the check for `sched_clock_running` status was omitted. The original sched_clock_cpu() had this guard to handle early boot, but it was not included in the new code path. This causes local_clock() to call sched_clock_local() before the scheduler clock data structure is properly initialized.

## Fix Summary

Add a check in local_clock() to return sched_clock() if sched_clock_running is not yet enabled, restoring the behavior from the previous sched_clock_cpu() implementation. This ensures correct timestamp progression during early kernel boot.

## Triggering Conditions

The bug occurs during early kernel boot before sched_clock_init() has run, when the `sched_clock_running` static branch is still false. In this state:
- The scheduler clock data structures (sched_clock_data) are not yet properly initialized
- local_clock() calls sched_clock_local() which operates on uninitialized data
- scd->clock reaches TICK_NSEC and gets stuck, causing all subsequent local_clock() calls to return the same stale timestamp
- This affects any code that uses local_clock() for timestamping during early boot (e.g., CONFIG_PRINTK_TIME dmesg timestamps)
- The timing window lasts from early boot until sched_clock_init() completes, which can be hundreds of milliseconds

## Reproduce Strategy (kSTEP)

Since this bug occurs during very early kernel boot before scheduler initialization, direct reproduction in kSTEP (which loads after boot) is challenging. However, we can simulate the conditions:

- **Setup**: 2+ CPUs (CPU 0 reserved for driver)
- **Strategy**: Manually manipulate the `sched_clock_running` static branch to simulate pre-init state
- **Steps**: 
  1. Save current state of sched_clock_running static branch
  2. Use kstep_write() to disable the static branch or directly access kernel symbols to set it false
  3. Call local_clock() multiple times via kstep_tick() to observe stale return values
  4. Use kstep callbacks (on_tick_begin) to log timestamp progression and detect when values get stuck at TICK_NSEC (1000000 ns)
  5. Restore the static branch state
- **Detection**: Compare consecutive local_clock() readings - bug triggers when values remain identical despite time passage
