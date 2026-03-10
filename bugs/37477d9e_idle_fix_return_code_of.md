# sched_ext: idle: Fix return code of scx_select_cpu_dfl()

- **Commit:** 37477d9ecabd15323700313724479bea94d0421e
- **Affected file(s):** kernel/sched/ext_idle.c
- **Subsystem:** sched_ext

## Bug Description

When the `scx_select_cpu_dfl()` function is called with the `SCX_PICK_IDLE_CORE` flag set, it searches for a fully idle SMT core for task placement. If no fully idle core is found, the buggy code would fall back to returning `prev_cpu`. However, `prev_cpu` is not guaranteed to be a fully idle SMT core, violating the API contract and allowing the scheduler to place tasks on a core that doesn't meet the caller's requirements.

## Root Cause

The function has an early exit point that returns `prev_cpu` when `SCX_PICK_IDLE_CORE` is set and the search for a fully idle core fails. This represents a semantic error: the function is supposed to either return a CPU meeting the specific requirements (a fully idle SMT core) or indicate failure. Returning `prev_cpu` breaks this contract by returning a CPU that does not necessarily satisfy the caller's explicit request.

## Fix Summary

The fix changes the return value from `prev_cpu` to `-EBUSY` when the `SCX_PICK_IDLE_CORE` requirement cannot be satisfied. This ensures the function properly signals failure to the caller, allowing them to handle the situation appropriately rather than receiving a CPU that doesn't meet their specification.

## Triggering Conditions

The bug occurs when `scx_select_cpu_dfl()` is called with the `SCX_PICK_IDLE_CORE` flag in an SMT topology where no fully idle cores are available. Specifically: the system must have SMT (simultaneous multithreading) enabled with multiple threads per core; tasks must be running on some threads of each core but leaving at least one thread per core idle (preventing any core from being fully idle); and a task placement request with `SCX_PICK_IDLE_CORE` flag must be made where `prev_cpu` is not part of a fully idle core. The function exhausts all search paths (LLC, NUMA node, global) without finding a fully idle core, triggering the problematic fallback path that incorrectly returns `prev_cpu` instead of signaling failure.

## Reproduce Strategy (kSTEP)

Create an SMT topology with at least 3 CPUs (excluding CPU 0 reserved for driver) using `kstep_topo_set_smt()` to configure SMT pairs like [1,2] and [3,4]. In `setup()`, create multiple tasks and use `kstep_task_pin()` to place one task on CPU 1 and one on CPU 3, leaving CPUs 2 and 4 idle but ensuring no core is fully idle. In `run()`, create a test task and attempt to place it using the sched_ext CPU selection with `SCX_PICK_IDLE_CORE` flag through direct kernel API calls or by triggering the scheduler path. Use `on_tick_begin()` callback to monitor CPU utilization and task placement. The bug is detected by observing that the function returns a valid CPU (prev_cpu) instead of `-EBUSY` when no fully idle core exists, violating the API contract and potentially causing incorrect task placement.
