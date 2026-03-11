# sched/debug: Fix updating of ppos on server write ops

- **Commit:** 6080fb211672aec6ce8f2f5a2e0b4eae736f2027
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core

## Bug Description

When `sched_fair_server_write()` encounters an error from `dl_server_apply_params()`, the buggy code overwrites the byte count with the error code and then modifies the file position pointer (ppos), which violates the standard file operation contract. Additionally, on success, the code unnecessarily modifies ppos even though the return value of `dl_server_apply_params()` is 0. This causes incorrect file position tracking and incorrect return values in sysfs write operations.

## Root Cause

The original code unconditionally reassigned `cnt = retval` when an error occurred (`if (retval) cnt = retval;`), which overwrote the actual byte count with the error code. Then `*ppos += cnt` would execute regardless of success/failure, incorrectly modifying the file position on errors. The standard file operation pattern requires that ppos only be updated on success and that errors are returned directly without modifying ppos.

## Fix Summary

The fix restructures the error handling to return the error code immediately when `retval < 0` occurs, bypassing the `*ppos += cnt` and return statement. On success, the original byte count is preserved and ppos is updated correctly. This ensures proper file position tracking and correct return values according to the standard file operation contract.

## Triggering Conditions

- The bug occurs during write operations to `/proc/sched_debug/cpu<N>/fair_server_runtime` or `/proc/sched_debug/cpu<N>/fair_server_period` sysfs files
- Must trigger an error from `dl_server_apply_params()` (e.g., invalid runtime/period values)
- The buggy code executes in the `sched_fair_server_write()` function in `kernel/sched/debug.c`
- Error conditions include: runtime/period validation failures, deadline server parameter misconfigurations
- On error, the original code incorrectly overwrites `cnt` with error code then updates `*ppos += cnt`
- On success with `retval = 0`, the original code unnecessarily modifies ppos

## Reproduce Strategy (kSTEP)

- Requires 2+ CPUs (CPU 0 reserved for driver)
- In `setup()`: Create cgroups and tasks, enable fair server debugging via sysctl if needed
- In `run()`: Use `kstep_write()` to write invalid values to `/proc/sched_debug/cpu1/fair_server_runtime`
- Write extremely large runtime values (exceeding fair_server_period_max ~4 seconds) to trigger `dl_server_apply_params()` errors
- Use `kstep_write()` with invalid period values (below fair_server_period_min 100μs) to trigger parameter validation failures
- Monitor file position tracking by attempting multiple sequential writes to same file descriptor
- Check return values vs actual bytes written - buggy version returns negative error codes instead of byte counts
- Use `on_tick_end()` callback to log scheduler state and verify fair server configuration remains consistent
