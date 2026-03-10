# sched/rt: Change the type of 'sysctl_sched_rt_period' from 'unsigned int' to 'int'

- **Commit:** 089768dfeb3ab294f9ab6a1f2462001f0f879fbb
- **Affected file(s):** kernel/sched/rt.c, kernel/sched/sched.h
- **Subsystem:** RT (Real-Time Scheduling)

## Bug Description

The `sysctl_sched_rt_period` variable was declared as `unsigned int`, but the calculus in `sched_rt_handler()` naturally uses `int` comparisons and operations. This type mismatch prevented proper implementation of upper bounds checking on the sysctl interface for the RT period parameter, allowing invalid values to potentially be set without proper validation.

## Root Cause

The variable was incorrectly declared as `unsigned int` while the handler function and bounds-checking logic expected an `int`. The type mismatch meant that the sysctl maxlen and bounds-checking constraints could not be correctly applied, bypassing validation of the parameter value on the sysctl interface.

## Fix Summary

The fix changes `sysctl_sched_rt_period` from `unsigned int` to `int` throughout the code (declaration, extern, and sysctl table), and updates the corresponding bounds-checking setup to properly constrain the value. This enables correct upper bounds validation on the sysctl interface and aligns the variable type with how it is naturally used in calculations.

## Triggering Conditions

The bug is triggered through the sysctl interface when attempting to write values to `/proc/sys/kernel/sched_rt_period_us`. The type mismatch between `unsigned int` declaration and `int`-based sysctl handler prevents proper bounds checking. Specifically:
- The sysctl table's `maxlen` field was set to `sizeof(unsigned int)` while the handler expected `sizeof(int)`
- Upper bounds validation in `sched_rt_handler()` could be bypassed for large values
- The related `sched_rt_runtime_us` bounds checking (should be ≤ period) was also affected
- Values exceeding INT_MAX could be incorrectly accepted, leading to integer overflow in subsequent calculations

## Reproduce Strategy (kSTEP)

Requires only 1 CPU (driver runs on CPU 0). No special task creation or topology setup needed:
- In `setup()`: No special configuration required
- In `run()`: 
  - Use `kstep_sysctl_write("kernel.sched_rt_period_us", "%u", UINT_MAX)` to attempt writing an invalid large value
  - Check if the write succeeds when it should fail (indicates the bug)
  - Also test `kstep_sysctl_write("kernel.sched_rt_runtime_us", "%d", period_value + 1)` with runtime > period
  - Read back the values with sysctl to verify what was actually stored
- Detection: Bug is present if extremely large period values are accepted without rejection
- Log the accepted vs expected values to demonstrate the validation bypass
