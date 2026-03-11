# sched/fair: Fix CPU bandwidth limit bypass during CPU hotplug

- **Commit:** af98d8a36a963e758e84266d152b92c7b51d4ecb
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

CPU controller bandwidth limits are not properly enforced during CPU hotplug operations. When a CPU goes offline, the kernel incorrectly unthrottles throttled cgroups across all active CPUs in the system, allowing processes to exceed their assigned quota limits. This causes a measurable performance impact: a workload with a 6.25% CPU limit (50000 out of 100000 units) runs for 14.43 seconds instead of the expected 10 seconds during hotplug operations.

## Root Cause

During CPU hotplug, scheduler domains are rebuilt and `cpu_attach_domain()` is called for every active CPU to update the root domain. This unconditionally invokes `rq_offline_fair()`, which calls `unthrottle_offline_cfs_rqs()` to unthrottle any throttled hierarchies. The function was unthrottling cfs_rqs on all CPUs being processed, not just the one actually going offline. Additionally, `runtime_remaining` was unconditionally set to 1 for all enabled cfs_rqs, even non-throttled ones, causing their quota to be quickly depleted.

## Fix Summary

The fix adds a check at the start of `unthrottle_offline_cfs_rqs()` to only proceed if the CPU is not in the active mask, ensuring unthrottling only happens for truly offline CPUs. The `runtime_remaining = 1` assignment is moved inside the throttled-cfs_rq condition, so it only applies when actually unthrottling a throttled hierarchy. This restores proper bandwidth limit enforcement during hotplug operations.

## Triggering Conditions

- **Scheduler subsystem**: CFS bandwidth control (`kernel/sched/fair.c`)
- **Code path**: CPU hotplug → `cpu_attach_domain()` → `rq_offline_fair()` → `unthrottle_offline_cfs_rqs()`
- **Required state**: Active throttled cgroups with CPU bandwidth limits (cpu.max set)
- **Topology**: Multi-CPU system (≥2 CPUs) where CPU hotplug operations occur
- **Timing**: Bug triggers during scheduler domain rebuilding when `cpu_attach_domain()` is called for every active CPU
- **Key condition**: Function incorrectly processes all CPUs instead of only the offline CPU
- **Observable impact**: Throttled cgroups get unthrottled across all active CPUs, bypassing bandwidth limits

## Reproduce Strategy (kSTEP)

- **CPUs needed**: At least 3 (CPU 0 reserved, need 2+ for hotplug simulation)
- **Setup**: Create throttled cgroup with bandwidth limit using `kstep_cgroup_create("test")` and `kstep_cgroup_set_weight()`. Create multiple CPU-bound tasks with `kstep_task_create()` and add to cgroup via `kstep_cgroup_add_task()`
- **Run sequence**: 1) Start tasks to consume bandwidth and trigger throttling, 2) Use `kstep_tick_repeat()` to let throttling occur, 3) Simulate CPU hotplug by manually calling `unthrottle_offline_cfs_rqs()` on active CPUs, 4) Monitor with `kstep_tick()` to observe unthrottling
- **Observation**: Use `on_tick_begin()` callback to log cfs_rq throttle status and `runtime_remaining` values
- **Detection**: Check if throttled cgroups become unthrottled when they shouldn't be, and verify `runtime_remaining` doesn't get reset to 1 for non-throttled cgroups during hotplug simulation
