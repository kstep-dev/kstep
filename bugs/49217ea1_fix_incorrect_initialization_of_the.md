# Fix incorrect initialization of the 'burst' parameter in cpu_max_write()

- **Commit:** 49217ea147df7647cb89161b805c797487783fc0
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (CFS bandwidth)

## Bug Description

When updating the `cpu.max` cgroup v2 parameter for the CPU subsystem, the `cpu.max.burst` value unexpectedly changes to an incorrect value. For example, after setting `cpu.max` to 2000000, the previously set `cpu.max.burst` value of 1000000 changes to 1000. This causes user-visible configuration corruption where modifying one cgroup parameter inadvertently corrupts another.

## Root Cause

The `cpu_max_write()` function calls `tg_get_cfs_burst()` to retrieve the burst value, which returns the value in microseconds. However, the function then passes this value directly to `tg_set_cfs_bandwidth()`, which expects the burst parameter in nanoseconds. This unit mismatch causes the burst value to be incorrectly converted and stored (microseconds treated as nanoseconds), resulting in a thousand-fold reduction in the actual burst value.

## Fix Summary

The fix replaces the call to `tg_get_cfs_burst(tg)` with direct access to `tg->cfs_bandwidth.burst`, which is already stored in nanoseconds. This eliminates the unit conversion mismatch and ensures the burst parameter retains its correct value when `cpu.max` is updated.

## Triggering Conditions

This bug triggers in the CFS bandwidth controller when modifying cgroup v2 `cpu.max` files after `cpu.max.burst` has been previously configured. The issue occurs in `cpu_max_write()` when it calls `tg_set_cfs_bandwidth()` with an existing burst value retrieved via `tg_get_cfs_burst()`. Since `tg_get_cfs_burst()` returns microseconds but `tg_set_cfs_bandwidth()` expects nanoseconds, any write to `cpu.max` will corrupt the burst value by dividing it by 1000. The bug manifests immediately upon any `cpu.max` update and is deterministic - no specific timing, task states, or CPU topologies are required beyond having cgroups v2 enabled with CFS bandwidth control.

## Reproduce Strategy (kSTEP)

Use at least 2 CPUs. In `setup()`: create a cgroup with `kstep_cgroup_create("testgroup")` and set initial burst value using `kstep_cgroup_write("testgroup", "cpu.max.burst", "1000000")`. In `run()`: set initial quota with `kstep_cgroup_write("testgroup", "cpu.max", "1000000 100000")`, then read and log the burst value via cgroup file reads. Next, update cpu.max again with `kstep_cgroup_write("testgroup", "cpu.max", "2000000 100000")` and read burst value again. Compare the before/after burst values - the bug manifests as burst changing from 1000000 to 1000 (1000x reduction). Use `kstep_cgroup_write()` for file operations and `TRACE_INFO()` to log the burst values at each step. The corruption is immediate and deterministic, requiring no specific callbacks or timing.
