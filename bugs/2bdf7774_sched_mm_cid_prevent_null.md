# sched/mm_cid: Prevent NULL mm dereference in sched_mm_cid_after_execve()

- **Commit:** 2bdf777410dc6e022d1081885ff34673b5dfee99
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** sched/mm_cid

## Bug Description

When `exec_binprm()` fails (e.g., due to a missing script interpreter), `sched_mm_cid_after_execve()` is still called in the cleanup path. For kernel tasks (like init), `current->mm` is NULL, but `sched_mm_cid_after_execve()` unconditionally calls `sched_mm_cid_fork()` without checking if `mm` exists, triggering a WARN_ON. This is easily reproduced by booting with an init that is a shell script whose interpreter doesn't exist in the initramfs.

## Root Cause

`sched_mm_cid_before_execve()` exits early when `mm` is NULL via `sched_mm_cid_exit()`, but `sched_mm_cid_after_execve()` unconditionally calls `sched_mm_cid_fork()` without the corresponding NULL check. When `exec_binprm()` fails, the task still has a NULL `mm`, and the after function dereferences it, causing a warning.

## Fix Summary

Add a NULL check for `t->mm` in `sched_mm_cid_after_execve()` before calling `sched_mm_cid_fork()`, matching the behavior of `sched_mm_cid_before_execve()` which already handles the NULL case.

## Triggering Conditions

The bug occurs in the scheduler's mm_cid (memory management context ID) subsystem during execve() cleanup. Triggering requires:
- A task with NULL `mm` field (kernel threads like init process)
- An execve() system call that fails (e.g., ENOENT for missing script interpreter)
- The cleanup path in `bprm_execve()` unconditionally calls `sched_mm_cid_after_execve()`
- `sched_mm_cid_after_execve()` directly calls `sched_mm_cid_fork()` without checking if `t->mm` is NULL
- `sched_mm_cid_fork()` expects a valid mm and triggers WARN_ON when passed NULL
- Race condition: `sched_mm_cid_before_execve()` exits early for NULL mm via `sched_mm_cid_exit()`, but the after function lacks this protection

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. In `setup()`, create a task representing a kernel thread using `kstep_task_create()`. 
In `run()`, directly manipulate the task's mm field to NULL to simulate a kernel thread state.
Use kernel function imports to directly call `sched_mm_cid_before_execve()` and `sched_mm_cid_after_execve()` on the task.
Monitor for WARN_ON triggers using kernel log output or by checking return values.
Alternatively, simulate execve() failure by creating a task, setting up an invalid executable path, and triggering the execve cleanup path.
Use `on_tick_begin()` callback to log task states and mm field values.
Detection: Check for kernel warnings in dmesg or WARN_ON triggers in the mm_cid subsystem functions.
Verification: Ensure the bug reproduces on buggy kernel and is fixed after applying the NULL check patch.
