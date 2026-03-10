# sched_ext: fix uninitialized ret on alloc_percpu() failure

- **Commit:** b0101ccb5b4641885f30fecc352ef891ed06e083
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

In `scx_alloc_and_add_sched()`, when `alloc_percpu()` fails, the function jumps to the error path without initializing the `ret` variable. This causes the function to return `ERR_PTR(0)` (or potentially an uninitialized garbage value), which violates the `ERR_PTR()` convention that expects only negative error codes. Callers relying on `IS_ERR()` and `PTR_ERR()` macros to check and extract error codes from the return value will be confused or may crash.

## Root Cause

The failure path for `alloc_percpu()` at line 4786 (before the fix) did not initialize the `ret` variable before calling `goto err_free_gdsqs`. The error path at the end of the function returns `ERR_PTR(ret)`, but `ret` was uninitialized in this particular failure scenario. Other failure paths in the function (e.g., at line 4777) properly set `ret = -ENOMEM` before jumping to `err_free_gdsqs`, but this one was missed.

## Fix Summary

The fix adds initialization of `ret = -ENOMEM` immediately before the `goto err_free_gdsqs` statement in the `alloc_percpu()` failure branch. This ensures the error path returns a proper error pointer with a valid negative error code, conforming to the `ERR_PTR()` convention and allowing callers to correctly detect and handle the failure.

## Triggering Conditions

This bug occurs during sched_ext scheduler initialization in `scx_alloc_and_add_sched()` when:
- The function successfully passes initial allocations (`alloc_exit_info`, `rhashtable_init`, `kcalloc` for global_dsqs)
- The per-node dispatch queue allocation loop completes successfully
- The `alloc_percpu(struct scx_sched_pcpu)` call fails due to memory pressure
- Control jumps to `err_free_gdsqs` with uninitialized `ret`, causing `ERR_PTR(0)` return
- Callers using `IS_ERR()` receive false negative, potentially leading to NULL pointer dereference

## Reproduce Strategy (kSTEP)

This bug requires memory allocation failure during scheduler extension initialization, which is challenging to reproduce with kSTEP's runtime scheduling focus. A potential approach:
- Use single CPU (minimum 2 CPUs: 0 for driver, 1 for test)  
- In `setup()`: Create memory pressure using `kstep_cgroup_create()` with tight memory limits
- In `run()`: Trigger sched_ext operations that invoke `scx_alloc_and_add_sched()`
- Force allocation failures by exhausting per-CPU memory pools via `kstep_task_create()` and `kstep_task_fork()`
- Monitor kernel logs for allocation failures and ERR_PTR(0) warnings
- Use custom callback to intercept scheduler extension initialization attempts
- Detect bug by checking return values in error conditions (requires kernel instrumentation)
- Note: This bug is primarily in initialization error handling, not runtime scheduling behavior
