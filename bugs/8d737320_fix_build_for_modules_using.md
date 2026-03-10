# sched: Fix build for modules using set_tsk_need_resched()

- **Commit:** 8d737320166bd145af70a3133a9964b00ca81cba
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A prior commit (adcc3bfa8806) added a tracepoint to the need_resched action, which is triggered when `set_tsk_need_resched()` is called. However, the required symbol `__trace_set_need_resched()` was not exported to modules, breaking the build of out-of-tree modules that relied on `set_tsk_need_resched()` being accessible—a regression from previously working code.

## Root Cause

When the tracepoint was introduced, the function `__trace_set_need_resched()` was added to wrap the tracepoint call, but it was not exported with `EXPORT_SYMBOL_GPL()`. This made the symbol unavailable to out-of-tree modules that depend on `set_tsk_need_resched()`, even though that function was previously accessible.

## Fix Summary

Export the `__trace_set_need_resched()` function using `EXPORT_SYMBOL_GPL()` to make it available to out-of-tree modules, restoring the ability to build and use modules that depend on `set_tsk_need_resched()`.

## Triggering Conditions

This is a build-time bug, not a runtime bug. The issue manifests when:
- Compiling out-of-tree kernel modules that call `set_tsk_need_resched()`
- The kernel has tracing enabled (CONFIG_TRACE_SCHED_RV=y)
- The referenced commit adcc3bfa8806 is present but this fix is missing
- Module build fails during linking due to undefined symbol `__trace_set_need_resched`
- Affects any external module using scheduler preemption APIs

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced using kSTEP since it's a build-time symbol export issue, not a runtime scheduler behavior. However, to verify the fix works:
- Attempt to build a kernel module that calls `set_tsk_need_resched()`
- On buggy kernels: link fails with "undefined symbol __trace_set_need_resched"
- On fixed kernels: module builds successfully
- Use kSTEP framework itself as test case - it likely calls this function
- Run `make kstep` with a buggy vs fixed kernel to demonstrate the issue
- No specific CPU topology, tasks, or scheduler state required
