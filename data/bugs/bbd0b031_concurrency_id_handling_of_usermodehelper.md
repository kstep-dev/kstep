# sched/rseq: Fix concurrency ID handling of usermodehelper kthreads

- **Commit:** bbd0b031509b880b4e9a880bb27ca2a30ad081ab
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (rseq, memory map concurrency IDs)

## Bug Description

The code asserts (via `WARN_ON_ONCE`) that `sched_mm_cid_after_execve()` and `sched_mm_cid_fork()` should never be called with kernel threads (`PF_KTHREAD` flag set) or with NULL `t->mm`. However, usermodehelper kthreads can call these functions when a binary execution attempt fails. This causes spurious warnings and test failures in real-world scenarios.

## Root Cause

The code incorrectly assumed that usermodehelper kthreads would never reach these scheduler concurrency ID functions. However, when a usermodehelper kthread fails to execute a binary, it may still attempt to clean up by calling these functions. Additionally, `sched_mm_cid_after_execve()` receives a task_struct with NULL `t->mm` in such failure scenarios, violating the assertion that the code made about valid inputs.

## Fix Summary

The fix removes the `PF_KTHREAD` check from the assertions and adds an explicit NULL `mm` check at the beginning of `sched_mm_cid_after_execve()` that returns early. This allows usermodehelper kthreads to safely call these functions without triggering false warnings.

## Triggering Conditions

The bug is triggered when a usermodehelper kthread attempts to execute a userspace binary and the execution fails. This happens in the following sequence:
- A usermodehelper kthread (with `PF_KTHREAD` flag) is created to execute a userspace program
- The kthread attempts to transition from kernel context to userspace via `execve()`
- The binary execution fails during `do_execveat_common()` in fs/exec.c
- During failure cleanup, `sched_mm_cid_after_execve()` is called on the kthread
- Since usermodehelper kthreads can have NULL `t->mm` and `PF_KTHREAD` flag, the old assertions would trigger `WARN_ON_ONCE`
- The race occurs specifically when usermodehelper execution fails and cleanup paths are executed

## Reproduce Strategy (kSTEP)

This bug requires simulating a usermodehelper kthread execution failure scenario. The reproduction strategy:
- Setup: Use 2 CPUs (driver on CPU 0, target kthread on CPU 1)
- Create a kthread using `kstep_kthread_create()` and simulate usermodehelper behavior
- In `setup()`: Create a test kthread and bind it to CPU 1 with `kstep_kthread_bind()`
- In `run()`: Use `kstep_kthread_start()` to activate the kthread
- Manually trigger the buggy path by calling `sched_mm_cid_after_execve()` on a kthread with NULL mm
- Use `on_tick_begin` callback to monitor task states and detect WARN_ON_ONCE triggers
- Check for kernel warning messages in the log output via `TRACE_INFO` 
- The bug is detected if the old code would trigger `WARN_ON_ONCE((t->flags & PF_KTHREAD) || !t->mm)`
- Verification: Ensure the kthread has `PF_KTHREAD` flag and NULL `mm` when the function is called
