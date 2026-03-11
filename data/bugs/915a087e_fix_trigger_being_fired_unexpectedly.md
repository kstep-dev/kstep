# psi: Fix trigger being fired unexpectedly at initial

- **Commit:** 915a087e4c47334a2f7ba2a4092c4bade0873769
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

When a new PSI trigger is created, it is initialized with a zero start_time and zero start_value. If the PSI group has already accumulated pressure data before the trigger is created, the trigger will fire unexpectedly in the next monitoring period, even though the actual pressure growth since trigger creation hasn't reached the threshold. This causes spurious trigger events that don't reflect real pressure changes.

## Root Cause

The `psi_trigger_create()` function initializes a new trigger's measurement window with hardcoded zeros via `window_reset(&t->win, 0, 0, 0)`. This causes the trigger to measure the difference from zero to the current accumulated group value, which includes all historical pressure data. Since existing pressure data may already exceed the threshold, the trigger fires on the first monitoring period regardless of actual pressure growth after trigger creation.

## Fix Summary

The fix initializes the trigger's window with the current clock time and the current accumulated PSI value: `window_reset(&t->win, sched_clock(), group->total[PSI_POLL][t->state], 0)`. This baseline ensures the trigger only measures pressure growth from the moment of creation, preventing spurious firings based on historical data.

## Triggering Conditions

The bug requires PSI to be enabled and pressure data to accumulate before trigger creation. Specifically:
- The PSI subsystem must be active and collecting pressure metrics for a resource (CPU, memory, or IO)
- Tasks must generate pressure stalls that cause `group->total[PSI_POLL][state]` to accumulate significant values
- A PSI trigger must be created via `/proc/pressure/*` with a threshold lower than the already-accumulated pressure value
- The trigger will incorrectly measure from zero to the current accumulated value, causing immediate firing
- This occurs during the next PSI polling period (typically within 2 seconds) regardless of actual pressure growth since trigger creation

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create memory pressure, accumulate PSI data, then create a trigger:
- In `setup()`: Create multiple tasks using `kstep_task_create()` and set up memory pressure scenario
- In `run()`: Start tasks with `kstep_task_wakeup()` to generate memory contention and accumulate PSI metrics
- Use `kstep_tick_repeat()` to let pressure build up over time, monitoring `/proc/pressure/memory` values
- Create a PSI trigger via `kstep_write("/proc/pressure/memory", "some 1000 100000", ...)` with threshold below accumulated pressure
- Use `kstep_tick_repeat()` to advance through the next PSI polling period and observe trigger firing
- Monitor trigger events through the PSI eventfd mechanism to detect spurious firing
- Compare behavior: trigger should fire immediately on buggy kernel, but only fire when actual new pressure exceeds threshold on fixed kernel
