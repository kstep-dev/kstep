# Fix tg->load when offlining a CPU

- **Commit:** f60a631ab9ed5df15e446269ea515f2b8948ba0c
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When a CPU is taken offline, the load contribution of its per-CPU CFS run queues to task groups remains in the global task group load_avg counter. This stale load is then used to calculate fair CPU share allocation among task groups, causing some cgroups to receive significantly less CPU time than they should after CPU hotplug operations. The bug manifests as unfair scheduling where one cgroup may get only 14% of CPU while another gets 83%, even when both have identical workloads.

## Root Cause

The task group's load_avg accumulates contributions from all CPUs' CFS run queues via `update_tg_load_avg()`. When a CPU goes offline, this function continues to be called, but the stale load contribution (stored in `tg_load_avg_contrib`) is never cleared from the task group's atomic counter. Since the offline CPU no longer participates in scheduling, its stale load inflates the task group's perceived load, causing the fair share calculation to underestimate the group's actual CPU demand on remaining online CPUs.

## Fix Summary

The fix adds a `clear_tg_load_avg()` function to zero out a CPU's load contribution when it goes offline, and introduces `clear_tg_offline_cfs_rqs()` as a CPU offline callback that clears the contribution of all task groups. Additionally, `update_tg_load_avg()` now skips updates from CPUs that are offline, preventing further stale contributions. These changes are applied in the `rq_offline_fair()` callback during CPU hotplug operations.

## Triggering Conditions

- Multiple task groups (cgroups) with CPU-intensive workloads running simultaneously
- CPU hotplug operations where CPUs are taken offline after accumulating significant load contributions
- Task groups that should receive equal CPU shares but experience unfair allocation due to stale offline CPU load
- The bug occurs in `update_tg_load_avg()` path where offline CPUs continue contributing to `tg->load_avg` atomic counter
- Timing: CPU goes offline while `tg_load_avg_contrib` contains non-zero values that remain uncleaned
- Most observable when CPU offline events happen during periods of high scheduler activity

## Reproduce Strategy (kSTEP)

**Note: kSTEP lacks direct CPU hotplug simulation, so this approach focuses on the core load accounting bug:**
- Setup: Need at least 3 CPUs (CPU 0 reserved for driver, CPUs 1-2 for workload)
- Create two cgroups: `kstep_cgroup_create("test_group_1")` and `kstep_cgroup_create("test_group_2")`
- Create CPU-intensive tasks: `kstep_task_create()` for each group, pin with `kstep_task_pin(task, 1, 2)`
- Add tasks to groups: `kstep_cgroup_add_task("test_group_1", task1->pid)`
- Run workload: `kstep_tick_repeat(100)` to accumulate `tg_load_avg_contrib` values
- Monitor via callback: Use `on_tick_end()` to log task group load values and check for stale contributions
- **Limitation**: Direct CPU offline requires kernel modification or external tooling beyond kSTEP's current API
- Detection: Check for imbalanced CPU allocation between cgroups with identical workloads after simulated offline events
