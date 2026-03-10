# sched_ext: Fix scx_enable() crash on helper kthread creation failure

- **Commit:** 7b6216baae751369195fa3c83d434d23bcda406a
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

A crash occurs in `scx_enable()` when helper kthread creation fails, particularly during signal handling (e.g., SIGQUIT). The crash manifests as a dereference of an error pointer rather than NULL. The bug is triggered when the sched_ext selftests runner terminates while a test is running, causing `kthread_run_worker()` to fail and return an error pointer instead of a valid task.

## Root Cause

The function `kthread_run_worker()` returns an `ERR_PTR()` on failure (not NULL), but `scx_alloc_and_add_sched()` only checks for NULL with `if (!sch->helper)`. When creation fails, the error pointer is not caught, and the error is not propagated to the caller. Subsequently, `scx_enable()` dereferences the error pointer in the error handling path, causing the crash.

## Fix Summary

The fix changes the error check from `if (!sch->helper)` to `if (IS_ERR(sch->helper))` and extracts the error with `PTR_ERR(sch->helper)` to propagate it to the return value. This allows `scx_enable()` to properly handle the failure and execute the correct error path instead of dereferencing an invalid pointer.

## Triggering Conditions

The bug occurs during sched_ext scheduler initialization in the `scx_enable()` -> `scx_alloc_and_add_sched()` code path. The trigger requires `kthread_run_worker()` to fail and return an `ERR_PTR()` instead of a valid kthread pointer. This failure can happen when:
- System receives signals (e.g., SIGQUIT) during scheduler initialization
- Kernel thread creation is under resource pressure or fails due to system state
- The sched_ext subsystem is being initialized while the system is shutting down
- Race condition where userspace terminates the sched_ext process during initialization
The bug manifests when the error pointer is not caught, leading to dereferencing of invalid memory in subsequent error handling paths.

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce directly with kSTEP since it occurs during sched_ext scheduler initialization, which is outside the normal CFS scheduler runtime that kSTEP typically tests. A reproduction approach would require:
- **CPUs needed:** At least 2 CPUs (CPU 0 reserved for driver)
- **Setup:** Use kSTEP to simulate system stress conditions that could cause kthread creation failures
- **Strategy:** Create multiple concurrent kthreads using `kstep_kthread_create()` in rapid succession to exhaust kernel thread resources
- **Trigger:** Use `kstep_task_create()` and `kstep_kthread_create()` to create resource pressure, then attempt to trigger sched_ext initialization
- **Detection:** Monitor for kernel crashes or error messages related to ERR_PTR dereferences in `scx_enable()`
- **Limitation:** Direct sched_ext testing may require extending kSTEP with sched_ext-specific APIs or using external tools to trigger the scheduler initialization failure scenario.
