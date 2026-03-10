# sched/clock: Avoid false sharing for sched_clock_irqtime

- **Commit:** 505da6689305b1103e9a8ab6636c6a7cf74cd5b1
- **Affected file(s):** kernel/sched/clock.c, kernel/sched/cputime.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

If the TSC is marked unstable before the late_initcall() phase, sched_clock_irqtime stays enabled even though the clock source is unreliable. This occurs during system initialization when tsc_init() detects an unsynchronized TSC and marks it unstable before sched_clock_init_late() runs. The irqtime accounting continues to use an unstable clock source, causing incorrect time accounting.

## Root Cause

The code path to disable sched_clock_irqtime is only triggered through the sched_clock_work workqueue, which is scheduled by clear_sched_clock_stable(). However, when TSC becomes unstable before late_initcall(), the static_key machinery is not yet initialized (static_key_count check fails), so __clear_sched_clock_stable() is never executed and the workqueue never runs. This leaves sched_clock_irqtime enabled despite the clock being marked unstable.

## Fix Summary

The fix adds an explicit call to disable_sched_clock_irqtime() in sched_clock_init_late() when the clock is detected as unstable. Additionally, sched_clock_irqtime is converted from a simple int variable to a static_key to avoid false sharing with frequently updated nohz data structures, requiring the corresponding update to the disable_sched_clock_irqtime() function to use static_branch_disable().

## Triggering Conditions

The bug requires early TSC instability detection during kernel initialization, specifically:
- TSC must be marked unstable via `tsc_init() -> mark_tsc_unstable()` before `late_initcall()` phase
- The unsynchronized TSC condition (`unsynchronized_tsc()` returns true) triggers `clear_sched_clock_stable()`
- Static key infrastructure not fully initialized yet (`static_key_count(&sched_clock_running.key) != 2`)
- `__sched_clock_stable_early` becomes false, preventing workqueue execution in `sched_clock_init_late()`
- `sched_clock_irqtime` remains enabled despite unstable clock, leading to incorrect time accounting
- The bug manifests as inconsistent interrupt time accounting using unreliable clock source

## Reproduce Strategy (kSTEP)

This bug occurs during early kernel initialization and cannot be reliably reproduced within kSTEP's runtime environment since it requires control over TSC stability detection before `late_initcall()`. However, we can validate the fix logic:
- Setup: Use 2+ CPUs (CPU 0 reserved for driver)
- In `setup()`: Create minimal task to trigger irqtime accounting paths via `kstep_task_create()`
- In `run()`: Force TSC instability simulation through direct kernel manipulation if possible
- Use `on_tick_begin()` callback to monitor sched_clock_irqtime static key state
- Check kernel logs with `TRACE_INFO()` for irqtime accounting behavior
- Validate that `disable_sched_clock_irqtime()` is called when clock becomes unstable
- Detect bug by observing continued irqtime accounting despite unstable clock indication
