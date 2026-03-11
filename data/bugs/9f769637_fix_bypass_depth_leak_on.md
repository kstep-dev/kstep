# sched_ext: Fix bypass depth leak on scx_enable() failure

- **Commit:** 9f769637a93fac81689b80df6855f545839cf999
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

When `scx_enable()` fails during task initialization (e.g., in `scx_cgroup_init()` or `scx_init_task()`), the bypass depth counter leaks, leaving the system permanently stuck in bypass mode. This occurs because `scx_enable()` increments bypass depth via `scx_bypass(true)` at startup, but on error it jumps to the error path without decrementing it, and then `scx_disable_workfn()` performs its own bypass increment/decrement cycle, leaving a net bypass depth of 1 instead of 0.

## Root Cause

The root cause is a mismatch in bypass depth management during error handling. `scx_enable()` calls `scx_bypass(true)` early to initialize in bypass mode, then calls `scx_bypass(false)` on success. However, if an error occurs before the `scx_bypass(false)` call (during task initialization), the error path (`err_disable`) does not call `scx_bypass(false)` to match the earlier `scx_bypass(true)`. Subsequently, `scx_disable_workfn()` performs its own `scx_bypass(true)` followed by `scx_bypass(false)`, resulting in a net bypass depth of 1 remaining instead of returning to 0.

## Fix Summary

The fix adds a boolean flag `scx_bypassed_for_enable` to track whether `scx_enable()` has called `scx_bypass(true)`. When `scx_disable_workfn()` runs (including on enable failure), it checks this flag and calls an additional `scx_bypass(false)` to balance the earlier increment, ensuring bypass depth returns to 0. The flag is set to true when `scx_bypass(true)` is called in `scx_enable()`, and cleared to false when `scx_bypass(false)` is called on successful initialization.

## Triggering Conditions

The bug occurs in the sched_ext subsystem when `scx_enable()` fails during task initialization phase after calling `scx_bypass(true)` but before `scx_bypass(false)`. Specifically:
- sched_ext scheduler enablement must be attempted via `scx_enable()`
- Failure must occur in task initialization functions (`scx_cgroup_init()` or `scx_init_task()`)
- Error path (`err_disable`) is taken without balancing the initial `scx_bypass(true)` call
- Subsequently, `scx_disable_workfn()` performs its own bypass cycle, leaving net depth of 1
- System remains permanently in bypass mode with `scx_bypass_depth = 1` instead of 0
- Failures after task initialization (e.g., in `scx_tryset_enable_state()`) are not affected as they properly call `scx_bypass(false)` before error path

## Reproduce Strategy (kSTEP)

This bug is specific to sched_ext subsystem which requires eBPF program attachment and is not directly reproducible through standard kSTEP APIs. A potential approach would be:
- Requires at least 2 CPUs (CPU 0 reserved for driver)
- Mock the sched_ext enabling process by directly manipulating `scx_bypass_depth` global variable
- In `setup()`: Use kernel module access to read initial `scx_bypass_depth` value
- In `run()`: Simulate the bug by incrementing bypass depth twice (mimicking `scx_enable()` + `scx_disable_workfn()` calls) without proper balancing
- Use `on_tick_begin()` callback to monitor bypass depth via direct kernel variable access
- Check that bypass depth becomes 1 instead of returning to 0, indicating permanent bypass mode
- Use `kstep_fail()` if bypass depth doesn't leak, `kstep_pass()` if the simulated bug reproduces the bypass depth leak condition
