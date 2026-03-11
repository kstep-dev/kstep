# sched/rt: Disallow writing invalid values to sched_rt_period_us

- **Commit:** 079be8fc630943d9fc70a97807feb73d169ee3fc
- **Affected file(s):** kernel/sched/rt.c
- **Subsystem:** RT

## Bug Description

A validation bypass allows negative values to be written to the `sched_rt_period_us` sysctl file. Because the underlying variable is declared as `unsigned int` but parsed using `proc_do_intvec()` which treats it as signed, negative input values get reinterpreted as large positive integers, bypassing the intended range check that requires the period to be greater than zero. This allows invalid configuration values to be accepted and later read back by userspace, potentially causing confusion.

## Root Cause

The `sysctl_sched_rt_period` variable is declared as `unsigned int`, but the sysctl handler uses `proc_do_intvec()` which parses it as a signed integer. After parsing, the validation check `if (sysctl_sched_rt_period <= 0) return EINVAL;` compares the unsigned variable against 0, but negative values written by userspace are already interpreted as large positive integers during the integer conversion, causing the validation to pass when it should fail.

## Fix Summary

The fix adds explicit minimum and maximum range constraints to the sysctl table entries using `extra1` and `extra2` fields (set to `SYSCTL_ONE` and `SYSCTL_INT_MAX` for the period), and changes the handler to use `proc_dointvec_minmax()` instead of `proc_do_intvec()`. This enforces range validation at the parsing stage before the values are assigned, preventing invalid negative values from being accepted.

## Triggering Conditions

This bug affects the sysctl validation subsystem in the RT scheduler. The triggering conditions are:
- A userspace process attempts to write a negative integer value to `/proc/sys/kernel/sched_rt_period_us` 
- The current sysctl handler uses `proc_do_intvec()` with post-parsing validation instead of `proc_dointvec_minmax()` 
- The vulnerable kernel versions lack proper range constraints in the sysctl table entry
- No special task states, CPU topology, or timing conditions are required - this is a pure sysctl validation bypass
- The bug manifests immediately upon the write operation, allowing the negative value to be stored as a large positive integer
- Reading the sysctl file back will return the corrupted large positive value instead of rejecting the original negative input

## Reproduce Strategy (kSTEP)

This bug can be reproduced with a minimal single-CPU setup focusing on sysctl manipulation:
- Only 1 CPU needed (CPU 0 reserved for driver is sufficient)
- No special task creation, cgroups, or topology configuration required in `setup()`
- In `run()`: Use `kstep_sysctl_write()` to write a negative value (e.g., "-1000") to "kernel.sched_rt_period_us"
- Immediately read back the value using another `kstep_sysctl_write()` or by reading `/proc/sys/kernel/sched_rt_period_us`
- Log the written and read-back values using `TRACE_INFO()`
- Bug is detected when: (1) the write operation succeeds without returning EINVAL, and (2) the read-back value is a large positive integer instead of the original negative value
- On fixed kernels, the write should fail with EINVAL and the sysctl value should remain unchanged
- No callbacks or complex scheduling observation needed - this is purely a sysctl interface validation test
