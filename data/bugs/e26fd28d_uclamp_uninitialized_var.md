# sched/uclamp: Fix a uninitialized variable warnings

- **Commit:** e26fd28db82899be71b4b949527373d0a6be1e65
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** EXT (energy-aware scheduling), uclamp

## Bug Description

The variables `util_min` and `util_max` in the `find_energy_efficient_cpu()` function could be used uninitialized in certain code paths. Specifically, when `uclamp_is_used()` returns false or when `uclamp_rq_is_idle(rq)` returns true, the conditional block that assigns values to these variables is skipped, leaving them uninitialized. These uninitialized variables are then passed to `util_fits_cpu()`, resulting in undefined behavior.

## Root Cause

The variables were declared without initialization, and their values were only conditionally set within an if block. If the condition was not met, the variables retained garbage values from the stack. This is a classic uninitialized variable bug that static analysis tools like smatch can detect.

## Fix Summary

The fix initializes `util_min` and `util_max` with default values (`p_util_min` and `p_util_max`) at the point of declaration, ensuring they always have valid values. The conditional code then selectively updates these variables only when needed, but the fallback initialization ensures they are always defined.

## Triggering Conditions

The bug occurs in the energy-aware scheduling (EAS) code path within `find_energy_efficient_cpu()`. It requires:
- Energy-aware scheduling enabled with performance domains configured
- A system with asymmetric CPU capacities (big.LITTLE or similar)
- Task wakeup on a CPU that triggers the EAS path
- Specific conditions where uclamp constraints are applied to runqueues but the per-domain `util_min`/`util_max` variables remain uninitialized
- The uninitialized variables are then passed to `util_fits_cpu()`, causing undefined behavior
- Static analysis tools like smatch can detect this reliably, but runtime manifestation depends on stack garbage values

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved for driver). Set up asymmetric CPU topology with different capacities:
- Use `kstep_cpu_set_capacity()` to create big.LITTLE configuration (e.g., CPUs 1-2 high capacity, 3-4 low capacity)
- Use `kstep_topo_init()` and `kstep_topo_set_cls()` to configure performance domains spanning different capacity CPUs
- Create tasks with uclamp constraints using `kstep_task_create()` and pin to specific CPUs with `kstep_task_pin()`
- Configure uclamp values via cgroup or task attributes to trigger EAS path
- Use `on_tick_begin` callback to monitor task placement and energy calculations
- Trigger task wakeups across performance domains to exercise `find_energy_efficient_cpu()`
- Monitor for inconsistent CPU selection or crashes due to garbage values in `util_fits_cpu()` calls
- Log uclamp utilization values and CPU capacity checks to detect undefined behavior
