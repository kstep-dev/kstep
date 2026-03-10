# sched/deadline: Fix reset_on_fork reporting of DL tasks

- **Commit:** f95091536f78971b269ec321b057b8d630b0ad8a
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/sched.h
- **Subsystem:** Deadline

## Bug Description

When `sched_getattr()` is called on a deadline task, it may incorrectly report the state of the `reset_on_fork` flag. This occurs when the flag was previously set via `sched_setattr()` with both `SCHED_FLAG_RESET_ON_FORK` and `SCHED_FLAG_KEEP_PARAMS` flags. The `SCHED_FLAG_KEEP_PARAMS` causes `__setscheduler()` to bail out early, preventing `dl_se->flags` from being updated. Subsequent calls to `sched_getattr()` then report stale deadline flags to userspace.

## Root Cause

The issue stems from a mismatch between which flags are stored in `dl_se->flags` (deadline-specific flags) and which flags are passed through `sched_attr.sched_flags` (a superset including general scheduler flags like `reset_on_fork`). When `__setparam_dl()` directly assigns all flags to `dl_se->flags` and `__getparam_dl()` directly reads from `dl_se->flags`, non-deadline flags like `reset_on_fork` can persist in `dl_se->flags` even after being modified in `p->sched_reset_on_fork`. This creates an inconsistency that causes incorrect reporting.

## Fix Summary

The fix introduces `SCHED_DL_FLAGS` macro to identify which flags are relevant to deadline scheduling, and ensures that only deadline-specific flags are stored in and retrieved from `dl_se->flags`. This prevents non-deadline flags from polluting the deadline flag field and corrupting the reported state when `sched_getattr()` is called.

## Triggering Conditions

The bug requires a deadline task where `sched_setattr()` is called with both `SCHED_FLAG_RESET_ON_FORK` and `SCHED_FLAG_KEEP_PARAMS` flags. The `SCHED_FLAG_KEEP_PARAMS` flag causes `__setscheduler()` to bail out early, preventing `__setparam_dl()` from being called to update `dl_se->flags`. This leaves stale flag values in the deadline entity. Subsequently, when `sched_getattr()` is called, `__getparam_dl()` directly copies the stale `dl_se->flags` value, overriding the correct `reset_on_fork` state stored in `p->sched_reset_on_fork`. The bug manifests as incorrect reporting of the `reset_on_fork` flag to userspace, where the reported value doesn't match the actual task state.

## Reproduce Strategy (kSTEP)

This bug involves the `sched_setattr()`/`sched_getattr()` syscall interface which is not directly exposed by kSTEP. The reproduction strategy would require extending kSTEP with syscall wrappers or accessing the underlying `__setparam_dl()` and `__getparam_dl()` functions directly. Create a deadline task using `kstep_task_create()` in `setup()`. In `run()`, manually call `__setparam_dl()` with a `sched_attr` structure containing both `SCHED_FLAG_RESET_ON_FORK` and `SCHED_FLAG_KEEP_PARAMS` in `sched_flags`, then call `__getparam_dl()` and verify that the returned `sched_flags` incorrectly reports the `reset_on_fork` state. The bug is detected by comparing the `reset_on_fork` flag in the returned attributes against the actual `p->sched_reset_on_fork` value - they should match but won't due to the stale `dl_se->flags`. This requires 1 CPU minimum, with logging in `run()` to show the flag mismatch.
