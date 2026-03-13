# LB: Load Balancer Fails to Spread Tasks Within LLC

**Commit:** `16b0a7a1a0af9db6e008fecd195fe4d6cb366d83`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.10-rc4
**Buggy since:** v5.5-rc1 (introduced by commit `0b0695f2b34a` "sched/fair: Rework load_balance()")

## Bug Description

The CFS load balancer's `calculate_imbalance()` function incorrectly uses a utilization-based migration strategy (`migrate_util`) instead of a task-count-based strategy (`migrate_task`) when balancing within scheduling domains that share package resources (i.e., within the same Last Level Cache / LLC). This causes tasks to pile up on some CPUs while other CPUs within the same LLC sit idle, dramatically increasing tail latency for periodic workloads.

The bug was introduced when commit `0b0695f2b34a` ("sched/fair: Rework load_balance()") rewrote the load balancing logic. In the new code, when the local group has spare capacity (`group_has_spare`) and the busiest group is overloaded (`group_type > group_fully_busy`), the code unconditionally enters a branch that uses `migrate_util` to compute the imbalance based on spare capacity. This is appropriate for cross-LLC or cross-NUMA balancing where capacity utilization is the right metric, but within an LLC domain, the wake-up path already tries to place tasks on idle CPUs within the same LLC. The load balancer should match this behavior and use `nr_running` (task count) to spread tasks evenly across idle CPUs.

The mismatch between the load balancer's strategy and the wake-up path's strategy means that when the load balancer runs at the LLC scheduling domain level, it may decide to migrate based on utilization rather than ensuring each CPU gets roughly the same number of tasks. Since utilization-based migration only looks at how much CPU capacity is being consumed, it can leave multiple tasks stacked on one CPU even though sibling CPUs in the LLC have no tasks at all. This is particularly damaging for latency-sensitive workloads like schbench, where tasks alternate between sleeping and running in bursts.

## Root Cause

The root cause is in `calculate_imbalance()` in `kernel/sched/fair.c`, within the `local->group_type == group_has_spare` branch. The buggy code is:

```c
if (local->group_type == group_has_spare) {
    if (busiest->group_type > group_fully_busy) {
        env->migration_type = migrate_util;
        env->imbalance = max(local->group_capacity, local->group_util) -
                         local->group_util;
        ...
        return;
    }
    ...
}
```

The `group_type` enum is ordered: `group_has_spare < group_fully_busy < group_misfit_task < group_asym_packing < group_imbalanced < group_overloaded`. So `busiest->group_type > group_fully_busy` matches any group that is misfit, asymmetric, imbalanced, or overloaded. When this condition is true, the function returns early with `migrate_util` as the migration type.

The problem is that this branch fires at ALL scheduling domain levels, including the LLC-level domain (which has the `SD_SHARE_PKG_RESOURCES` flag set). At the LLC level, CPUs share the same cache, and the scheduler's wake-up path (via `select_idle_sibling()`) already attempts to place waking tasks on idle CPUs within the LLC. The load balancer should complement this by using task-count-based balancing (`nr_running` / idle CPU count) at the LLC level, ensuring tasks are evenly distributed to idle CPUs.

When the buggy code triggers at the LLC level, it computes imbalance as the spare utilization capacity of the local group. This can result in migrating too few tasks (if spare capacity is small relative to the number of stacked tasks) or using the wrong migration strategy entirely. With `migrate_util`, the `detach_tasks()` function selects tasks based on their utilization contribution, which may not move enough tasks to fill all idle CPUs. In contrast, the fallthrough path (when the `if` condition is false) uses `migrate_task` with an imbalance computed from `idle_cpus` counts: `(local->idle_cpus - busiest->idle_cpus) >> 1`, which directly targets evening out the number of idle CPUs across groups.

The critical distinction is: at the LLC level, the goal should be to spread tasks so every CPU gets roughly the same number of runnable tasks (matching what `select_idle_sibling()` does on the wakeup path). At higher domain levels (cross-LLC, cross-NUMA), the goal should be to fill capacity, because moving a task across LLC boundaries has cache penalty costs and utilization-based balancing is more appropriate.

## Consequence

The observable consequence is a massive increase in scheduling latency for the tail percentiles (95th, 99th) of periodic workloads. On the test system (HiKey 8-core ARM64, which has all CPUs in a single LLC), running schbench (a latency benchmark using 2 message groups × 4 threads):

- **Buggy kernel**: 95th percentile latency = 4,152 µs; 99th percentile = 14,288 µs
- **Fixed kernel**: 95th percentile latency = 78 µs; 99th percentile = 94 µs

This is a **53× improvement at p95** and a **152× improvement at p99**. The bug causes individual tasks to wait much longer than necessary before getting scheduled, because the load balancer fails to move tasks off an overloaded CPU onto idle CPUs in the same LLC. Instead of balancing based on the number of running tasks, it tries to balance based on utilization, which may compute zero imbalance even when one CPU has 4 tasks and an adjacent CPU has 0.

This is a performance/latency degradation bug, not a crash or data corruption. However, the severity is extreme for latency-sensitive workloads. Systems with all CPUs sharing a single LLC (common in mobile/embedded ARM SoCs and some server chips) are most heavily affected. Systems where LLC boundaries coincide with scheduling domain boundaries at a higher level would see less impact at the LLC level but might still be affected if load balancing at the LLC domain is the primary balancing path.

## Fix Summary

The fix adds a single additional condition to the check in `calculate_imbalance()`:

```c
// Before (buggy):
if (busiest->group_type > group_fully_busy) {

// After (fixed):
if ((busiest->group_type > group_fully_busy) &&
    !(env->sd->flags & SD_SHARE_PKG_RESOURCES)) {
```

The added condition `!(env->sd->flags & SD_SHARE_PKG_RESOURCES)` ensures that the `migrate_util` branch is only taken when the scheduling domain does NOT share package resources (i.e., not within the same LLC). When `SD_SHARE_PKG_RESOURCES` is set (LLC-level domain), the code falls through to the subsequent logic which handles task-count-based balancing:

1. If `busiest->group_weight == 1` or `sds->prefer_sibling` is set, it computes the imbalance as half the difference in running task counts: `(busiest->sum_nr_running - local->sum_nr_running) >> 1`.
2. Otherwise (the default case for LLC-level balancing), it uses idle CPU counts: `(local->idle_cpus - busiest->idle_cpus) >> 1`, directly targeting an even distribution of idle CPUs.

This fix aligns the load balancer with the wake-up path's philosophy at the LLC level: try to give every CPU in the LLC roughly the same number of tasks, rather than trying to fill capacity. The fix is correct because the `SD_SHARE_PKG_RESOURCES` flag precisely identifies the LLC scheduling domain on all architectures, and the fallthrough path already contained the appropriate task-spreading logic — it just wasn't being reached when the busiest group was overloaded.

## Triggering Conditions

The bug triggers under the following conditions:

1. **Scheduling domain with `SD_SHARE_PKG_RESOURCES` flag**: The system must have a scheduling domain where CPUs share package resources (LLC). This is the standard LLC scheduling domain present on virtually all multi-core systems. On ARM SoCs like the HiKey (8 cores sharing a single LLC), this is the primary scheduling domain. On x86 systems, this is typically the domain spanning cores within a socket that share L3 cache.

2. **Busiest group is overloaded**: The busiest scheduling group within the LLC domain must have `group_type > group_fully_busy`. In practice, this means one group of CPUs has accumulated enough tasks that the scheduler classifies it as `group_overloaded` (or any intermediate state like `group_misfit_task`, `group_asym_packing`, or `group_imbalanced`). The most common trigger is `group_overloaded`, which happens when a group's `nr_running > group_weight` (more tasks than CPUs).

3. **Local group has spare capacity**: The local group (where the load balancer is running) must be classified as `group_has_spare`, meaning it has idle CPUs or light load.

4. **Workload pattern**: The bug is most pronounced with periodic, bursty workloads like schbench, where multiple tasks wake up simultaneously and compete for CPUs. Specifically, more running tasks than CPUs in the busiest group, combined with idle CPUs in the local group within the same LLC, maximizes the impact.

5. **Multi-core system with shared LLC**: The test case uses 8 cores with a shared LLC, running 8 tasks (2 groups × 4 threads). The key is having enough tasks to create overload on some CPUs while others remain idle within the same LLC.

The bug is highly reproducible: it occurs on every load balance tick where the above conditions hold within an LLC domain. The schbench benchmark reliably demonstrates it because it creates exactly the right pattern of bursty wakeups that expose the task stacking behavior.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reason:

### 1. Why it cannot be reproduced

**Kernel version too old (pre-v5.15).** The fix commit `16b0a7a1a0af9db6e008fecd195fe4d6cb366d83` was merged into **v5.10-rc4**. kSTEP supports Linux v5.15 and newer only. The bug existed from v5.5-rc1 (when `0b0695f2b34a` was merged) through v5.10-rc3. By v5.15, the bug is already fixed and the buggy code path no longer exists.

### 2. What would need to change

If the kernel version constraint were lifted, reproducing this bug in kSTEP would require:

- **Topology setup**: Configure a multi-CPU topology (e.g., 8 CPUs) where all CPUs share an LLC (i.e., a scheduling domain with `SD_SHARE_PKG_RESOURCES`). This can be done with `kstep_topo_init()` and `kstep_topo_set_mc()` / `kstep_topo_apply()`.
- **Task creation**: Create 8+ CFS tasks and pin subsets to different groups of CPUs to create a load imbalance within the LLC domain — some CPUs overloaded (more tasks than CPUs) and others idle.
- **Observation**: Hook `on_sched_balance_begin` / `on_sched_balance_selected` to observe the `migration_type` and `imbalance` values chosen by `calculate_imbalance()`. On the buggy kernel, expect `migrate_util` at the LLC level; on the fixed kernel, expect `migrate_task` with imbalance based on idle CPU counts.
- **Detection**: After several tick cycles, check whether tasks are evenly spread across CPUs by reading `nr_running` for each CPU. On the buggy kernel, some CPUs would remain overloaded while LLC siblings are idle.

### 3. Alternative reproduction methods

Outside kSTEP, the bug can be reproduced on actual hardware or a QEMU VM running kernel v5.5 through v5.10-rc3 by running the schbench benchmark:

```
schbench -m 2 -t 4 -s 10000 -c 1000000 -r 10
```

Compare the 95th and 99th percentile latencies between the buggy and fixed kernels. The buggy kernel will show latencies in the millisecond range for tail percentiles, while the fixed kernel shows sub-100µs latencies.

### 4. Summary

This is a kernel-version-too-old case. The fix shipped in v5.10-rc4, well before kSTEP's minimum supported version of v5.15. No kSTEP driver can be written because the buggy kernel cannot be built within kSTEP's supported range.
