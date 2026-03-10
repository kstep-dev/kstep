# sched_ext: Fix scx_kick_pseqs corruption on concurrent scheduler loads

- **Commit:** 05e63305c85c88141500f0a2fb02afcfba9396e1
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

When loading a BPF scheduler while another scheduler is already running, the `alloc_kick_pseqs()` function would be called and overwrite the previously allocated `scx_kick_cpus_pnt_seqs` arrays. This causes data corruption of the scheduler's internal state and leads to incorrect behavior or potential crashes.

## Root Cause

The `alloc_kick_pseqs()` call was placed before the `scx_enable_state()` check in the `scx_enable()` function. This means the allocation would occur even if the enable state check would subsequently fail (i.e., when another scheduler is already running). Since the arrays had already been allocated on a prior successful scheduler load, reallocating them would corrupt or leak the previous pointers.

## Fix Summary

The fix moves the `alloc_kick_pseqs()` call to occur after the `scx_enable_state()` check, ensuring that arrays are only allocated when a scheduler can actually be loaded. If another scheduler is already active, the function returns early without reallocating, preserving the existing arrays.

## Triggering Conditions

The bug occurs in the sched_ext subsystem when attempting to load a BPF scheduler while another scheduler is already active. The specific conditions required are:
- A sched_ext BPF scheduler must already be successfully loaded and running
- A second attempt to load another BPF scheduler (via `scx_enable()`) must occur
- The `alloc_kick_pseqs()` function executes before the `scx_enable_state() != SCX_DISABLED` check
- This overwrites the existing `scx_kick_cpus_pnt_seqs` arrays without freeing them first
- The subsequent state check fails with `-EBUSY`, but the corruption has already occurred
- The arrays may point to invalid memory or cause use-after-free when the scheduler operates

## Reproduce Strategy (kSTEP)

This bug requires testing concurrent scheduler loads, which is challenging to reproduce directly with kSTEP since it focuses on scheduler behavior rather than BPF scheduler loading mechanisms. However, a kSTEP driver could verify the fix by:
- Use 2+ CPUs to allow scheduler operations while driver runs on CPU 0
- In `setup()`, create minimal task workload with `kstep_task_create()` and `kstep_task_wakeup()`
- In `run()`, simulate the conditions by directly calling internal functions if accessible via KSYM_IMPORT
- Monitor for corruption by checking `scx_kick_cpus_pnt_seqs` pointer values before/after simulated concurrent loads
- Use `on_tick_begin()` callback to detect scheduler instability or crashes after potential corruption
- Log pointer addresses and memory contents to detect overwrites: `TRACE_INFO("pseqs ptr: %p", scx_kick_cpus_pnt_seqs)`
- Execute `kstep_tick_repeat(100)` to stress-test scheduler after simulated corruption
- Verify that fixed kernel prevents the double allocation by checking error returns
