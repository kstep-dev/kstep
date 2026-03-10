# sched/fair: Null terminate buffer when updating tunable_scaling

- **Commit:** 703066188f63d66cc6b9d678e5b5ef1213c5938e
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core

## Bug Description

Users cannot write to `/sys/kernel/debug/sched/tunable_scaling` sysctl file. Attempts to set the value fail with "Invalid argument" error even when providing valid values like 0. The issue prevents dynamic adjustment of the tunable_scaling parameter at runtime, breaking a key system configuration mechanism.

## Root Cause

The `sched_scaling_write()` function copies user input into a stack buffer but fails to null-terminate it before passing to `kstrtouint()`. The string parsing function expects a null-terminated string and fails to correctly parse the input without it, causing the write operation to be rejected.

## Fix Summary

The fix adds null termination to the buffer (`buf[cnt] = '\0'`) after copying user data and before parsing. It also introduces validation to check that the parsed value falls within the valid range `[0, SCHED_TUNABLESCALING_END)` before assignment. This ensures both correct parsing and proper range validation.

## Triggering Conditions

The bug occurs in the debugfs interface `/sys/kernel/debug/sched/tunable_scaling` when:
- The `sched_scaling_write()` function is called through any write operation to the file
- User input is copied into a 16-byte stack buffer via `copy_from_user()`
- The buffer is passed to `kstrtouint()` without null termination
- `kstrtouint()` expects a null-terminated string and fails to parse, returning `-EINVAL`
- This affects any write attempt, regardless of whether the input value (0, 1, 2) is valid
- The bug is deterministic - every write operation to this debugfs file will fail
- Requires CONFIG_SCHED_DEBUG to be enabled for the debugfs interface to exist

## Reproduce Strategy (kSTEP)

Reproducing this bug with kSTEP requires triggering the debugfs write operation:
- **CPU requirements**: Single CPU sufficient (CPU 0 reserved for driver)
- **Setup**: No special task/cgroup/topology setup needed - this is a pure interface bug
- **Trigger method**: Use `kstep_write()` to write directly to `/sys/kernel/debug/sched/tunable_scaling`
- **Test sequence**: 
  1. `kstep_write("/sys/kernel/debug/sched/tunable_scaling", "0", 1)` - should fail with -EINVAL
  2. `kstep_write("/sys/kernel/debug/sched/tunable_scaling", "1", 1)` - should fail with -EINVAL  
  3. `kstep_write("/sys/kernel/debug/sched/tunable_scaling", "2", 1)` - should fail with -EINVAL
- **Detection**: Check return value from `kstep_write()` calls - all should return error
- **Verification**: On fixed kernel, the same writes should succeed (return positive byte count)
- **Alternative**: Use `kstep_sysctl_write()` if it supports debugfs paths, or implement custom write via file operations
