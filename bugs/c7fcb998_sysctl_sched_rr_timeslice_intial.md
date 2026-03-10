# Fix sysctl_sched_rr_timeslice intial value

- **Commit:** c7fcb99877f9f542c918509b2801065adcaf46fa
- **Affected file(s):** kernel/sched/rt.c
- **Subsystem:** RT

## Bug Description

The initial value of `sysctl_sched_rr_timeslice` contains a 10% rounding error when `CONFIG_HZ_300=y`. The sysctl file reports an incorrect value (90 instead of 100), causing the sched_rr_get_interval test to fail when comparing the actual time quantum returned by the syscall with the value advertised in `/proc/sys/kernel/sched_rr_timeslice_ms`.

## Root Cause

The original calculation `(MSEC_PER_SEC / HZ) * RR_TIMESLICE` performs integer division before multiplication. With HZ=300, this evaluates to `(1000 / 300) * 30 = 3 * 30 = 90`, where the division truncates the result to 3, losing precision. The order of operations causes premature truncation when MSEC_PER_SEC is not a multiple of HZ.

## Fix Summary

The fix reverses the order of operations to `(MSEC_PER_SEC * RR_TIMESLICE) / HZ`, ensuring all multiplications occur before division. This avoids truncation losses and correctly computes the value as `(1000 * 30) / 300 = 100` for HZ=300.

## Triggering Conditions

This bug triggers during kernel initialization when `CONFIG_HZ_300=y`. The RT scheduler subsystem initializes `sysctl_sched_rr_timeslice` using the flawed calculation `(MSEC_PER_SEC / HZ) * RR_TIMESLICE`. The integer division `1000/300=3` truncates before multiplication, yielding 90ms instead of 100ms. The bug manifests whenever `/proc/sys/kernel/sched_rr_timeslice_ms` is read or `sched_rr_get_interval()` syscall is invoked on SCHED_RR tasks. No specific task states, CPU topology, or race conditions are required - the incorrect value is baked in at compile/init time.

## Reproduce Strategy (kSTEP)

Requires any CPU count (≥2). In `setup()`, verify HZ=300 via `kstep_sysctl_write()` or kernel version check. In `run()`, create a SCHED_RR task with `kstep_task_create()` and `kstep_task_fifo()` (use RT scheduling). Read the sysctl value by checking `/proc/sys/kernel/sched_rr_timeslice_ms` through `kstep_write()` and file operations. Compare the sysctl value (should be 90 on buggy kernel) with the expected 100ms. Optionally use syscall interface by implementing `sched_rr_get_interval()` call on the RT task. Detection: `kstep_fail()` if sysctl shows 90, `kstep_pass()` if it shows 100. No callbacks or timing dependencies needed since this is a static initialization bug.
