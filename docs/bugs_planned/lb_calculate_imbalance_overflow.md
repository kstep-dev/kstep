# LB: Integer Overflow in calculate_imbalance When Local Load Exceeds System Average

**Commit:** `91dcf1e8068e9a8823e419a7a34ff4341275fb70`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.3-rc7
**Buggy since:** `0b0695f2b34a` ("sched/fair: Rework load_balance()"), merged in v5.5-rc1

## Bug Description

The `calculate_imbalance()` function in the CFS load balancer computes the amount of load to migrate from the busiest scheduling group to the local group during periodic load balancing. When both the local group and busiest group are overloaded (or the local group is `group_fully_busy` and the busiest is `group_overloaded`), the function attempts to equalize loads toward the system-wide average by computing:

```c
env->imbalance = min(
    (busiest->avg_load - sds->avg_load) * busiest->group_capacity,
    (sds->avg_load - local->avg_load) * local->group_capacity
) / SCHED_CAPACITY_SCALE;
```

However, when the local group is `group_fully_busy` (not yet `group_overloaded`), the code computes `local->avg_load` and `sds->avg_load` just before reaching this formula. There is a check that `local->avg_load < busiest->avg_load`, but there is NO check that `local->avg_load < sds->avg_load` before the subtraction `(sds->avg_load - local->avg_load)` is performed. Since both `sds->avg_load` and `local->avg_load` are `unsigned long`, when the local group's average load exceeds the system-wide average load, the subtraction wraps around to a very large positive number. This causes the `min()` operation to select this enormous value (or the other operand, depending on magnitudes), and the resulting `env->imbalance` becomes a meaninglessly large number.

This situation arises in practice when the workload is asymmetric across scheduling groups — for example, when cgroup bandwidth throttling limits some tasks in one group while other groups run unrestricted. The throttled tasks may produce a local group whose weighted load per capacity exceeds the system-wide average, even though the busiest group has even more load.

The bug was reported by Tingjia Cao, who demonstrated the issue on a dual-socket Intel Xeon Silver 4114 system (20 physical cores) using cgroup-v2 bandwidth throttling. With bandwidth-limited tasks sleeping occasionally and unrestricted tasks running continuously, the `local->avg_load` reliably exceeded `sds->avg_load`, triggering the unsigned integer underflow in the imbalance calculation.

## Root Cause

The root cause is a missing bounds check in the `calculate_imbalance()` function in `kernel/sched/fair.c`. Specifically, the function handles the case where `local->group_type < group_overloaded` (i.e., local is `group_fully_busy`) and the busiest group is `group_overloaded`. In this path, the code computes:

```c
local->avg_load = (local->group_load * SCHED_CAPACITY_SCALE) / local->group_capacity;
```

It then checks:

```c
if (local->avg_load >= busiest->avg_load) {
    env->imbalance = 0;
    return;
}
```

This correctly prevents pulling load when the local group is already more loaded than the busiest. After this check passes (meaning `local->avg_load < busiest->avg_load`), the system-wide average is computed:

```c
sds->avg_load = (sds->total_load * SCHED_CAPACITY_SCALE) / sds->total_capacity;
```

The function then falls through to the final imbalance computation:

```c
env->imbalance = min(
    (busiest->avg_load - sds->avg_load) * busiest->group_capacity,
    (sds->avg_load - local->avg_load) * local->group_capacity
) / SCHED_CAPACITY_SCALE;
```

The critical invariant for this formula to work correctly is: `busiest->avg_load > sds->avg_load > local->avg_load`. The first inequality (`busiest->avg_load > sds->avg_load`) is guaranteed by the fact that the busiest group has more load than the average. However, the second inequality (`sds->avg_load > local->avg_load`) is NOT guaranteed. When the local group's load per capacity exceeds the system-wide average — which happens naturally when the local group is dense with high-weight tasks or when other groups have bandwidth-throttled tasks — the subtraction `(sds->avg_load - local->avg_load)` produces an unsigned underflow.

For concrete example from the bug report: `busiest->avg_load = 236`, `sds->avg_load = 103`, `local->avg_load = 104`. The subtraction `(103 - 104)` on `unsigned long` yields `0xFFFFFFFFFFFFFFFF` (on 64-bit), which when multiplied by `local->group_capacity` and divided by `SCHED_CAPACITY_SCALE`, produces a massive imbalance value (reported as 133 in the specific example, due to truncation and capacity scaling). The expected correct behavior would be that since `local->avg_load (104) > sds->avg_load (103)`, the local group is already above average and should NOT pull any additional load — the imbalance should be 0.

## Consequence

The primary consequence is incorrect load migration decisions during CFS load balancing. When the imbalance overflows to a large value:

1. **Unnecessary task migration**: The load balancer proceeds to call `find_busiest_queue()` and `detach_tasks()` with a hugely inflated imbalance. Since `env->migration_type` is set to `migrate_load`, the balancer will attempt to pull tasks whose `h_load` (hierarchical load) sums up to the inflated imbalance value. This means it may migrate one or more tasks from the busiest group to the local group, even though the local group is already above the system-wide average load. After the migration, the local group becomes even more overloaded relative to the system average.

2. **Amplified imbalance**: Rather than converging toward a balanced state, the scheduler actively worsens the imbalance. The migrated tasks increase the local group's load further above the system average, which can trigger further rounds of incorrect balancing in subsequent ticks. This creates a pathological cycle where tasks bounce between groups, consuming CPU time on migration overhead (cache invalidation, runqueue lock contention, IPI signaling) without improving actual workload distribution.

3. **Performance degradation**: On the reporter's 20-core dual-socket system with bandwidth-throttled cgroups, this manifested as measurably incorrect scheduling behavior where the trace showed migrations happening with `env->nr_balanced_fail = 0` (meaning the balancer confidently chose to migrate), and tasks with `h_load = 85` being migrated against an imbalance of 133, when the true imbalance should have been 0 or negative. While this does not cause a kernel crash or panic, it degrades throughput and latency for affected workloads, particularly mixed cgroup workloads with bandwidth throttling on multi-socket systems.

## Fix Summary

The fix adds a single early-return check in the `local->group_type < group_overloaded` path of `calculate_imbalance()`. After computing both `local->avg_load` and `sds->avg_load`, and before falling through to the `min()` formula, the fix inserts:

```c
/*
 * If the local group is more loaded than the average system
 * load, don't try to pull any tasks.
 */
if (local->avg_load >= sds->avg_load) {
    env->imbalance = 0;
    return;
}
```

This check ensures that the invariant `sds->avg_load > local->avg_load` holds before the subtraction `(sds->avg_load - local->avg_load)` is computed. When the local group is already above the system-wide average, pulling more load would only increase the imbalance, so setting `env->imbalance = 0` and returning early is the correct behavior.

The fix is minimal, correct, and complete. It mirrors the existing check `if (local->avg_load >= busiest->avg_load)` that prevents pulling when local exceeds the busiest, and places the new check at the logically correct point — after `sds->avg_load` has been computed but before it is used in an unsigned subtraction. The fix does NOT affect the `group_overloaded` vs `group_overloaded` path (where both groups are overloaded), because in that case both `avg_load` values were already computed earlier in `update_sd_lb_stats()` and the condition is naturally satisfied by the busiest-group selection logic.

## Triggering Conditions

The bug requires the following precise conditions to trigger:

1. **Scheduling domain with at least two scheduling groups**: A multi-core or multi-socket topology is needed so that load balancing occurs between different scheduling groups. The reporter used a dual-socket system (20 cores), but any topology with at least 2 groups at some domain level (e.g., MC domain with 2+ physical packages, or even 4 CPUs with 2 MC groups) suffices.

2. **Busiest group is `group_overloaded`**: The busiest scheduling group must have `group_type == group_overloaded`. This occurs when the group has more running tasks than CPUs, OR when `sum_nr_running > group_weight` (more tasks than cores). This is achieved by running many tasks pinned to or naturally placed on one group's CPUs.

3. **Local group is `group_fully_busy`**: The local scheduling group must have `group_type == group_fully_busy`. This means all CPUs in the local group are busy (no idle CPUs), but the group is not overloaded (running tasks <= group weight). Having exactly as many tasks as CPUs in the local group achieves this.

4. **`local->avg_load > sds->avg_load`**: This is the critical overflow condition. The local group's average load per capacity must exceed the system-wide average load per total capacity. This can happen when:
   - Cgroup bandwidth throttling (`cpu.max`) limits some tasks' effective CPU utilization, reducing `total_load` relative to what it would be without throttling, which lowers `sds->avg_load`.
   - The local group runs tasks without bandwidth limits or with higher weight, giving it relatively high `group_load` per `group_capacity`.
   - The bandwidth-throttled tasks periodically sleep (as their quota is exhausted), reducing `sds->total_load` below what would balance the groups.

5. **`local->avg_load < busiest->avg_load`**: This must also hold, otherwise the existing check catches it. The busiest group must have higher average load than the local group — the busiest is genuinely more loaded, but the local is above the system average.

The reproduction from the bug report uses two cgroups: `t0` with `cpu.max = 100000 10000` (10% bandwidth limit, i.e., 10000µs quota per 100000µs period) and `t1` with no bandwidth limit. In cgroup `t0`, 3 parent tasks each clone 9 children that sleep occasionally, creating moderate intermittent load. In `t1`, 1 parent task clones 10 continuously running children. The bandwidth throttling of `t0` tasks causes them to be periodically throttled, lowering the system-wide average load while the local group (containing unrestricted tasks) maintains high load. The condition triggers reliably on a 20-core dual-socket system and can be observed via tracing `calculate_imbalance()`.

## Reproduce Strategy (kSTEP)

This bug is reproducible with kSTEP. The core requirement is creating an asymmetric load distribution across scheduling groups where one group is `group_fully_busy` with `avg_load > sds->avg_load` while the other is `group_overloaded`. Cgroup bandwidth throttling via `cpu.max` is the key mechanism to create the right conditions, and kSTEP fully supports this via `kstep_cgroup_write()`.

### Step 1: Topology Setup

Configure a 4-CPU system with two scheduling groups at the MC domain level. Each group contains 2 CPUs:

```c
kstep_topo_init();
const char *mc[] = {"0", "1-2", "1-2", "3-4", "3-4"};
kstep_topo_set_mc(mc, 5);
kstep_topo_apply();
```

This gives CPUs 1-2 in one MC group and CPUs 3-4 in another. CPU 0 is reserved for the driver.

### Step 2: Cgroup Configuration

Create two cgroups:
- `t0`: with bandwidth throttling via `cpu.max` set to a low quota (e.g., `"10000 100000"` = 10% of a CPU)
- `t1`: no bandwidth limit (default), or explicitly set to `"max 100000"`

```c
kstep_cgroup_create("t0");
kstep_cgroup_create("t1");
kstep_cgroup_write("t0", "cpu.max", "10000 100000");
```

### Step 3: Task Creation and Placement

Create tasks to produce the desired group states:

- **Local group (CPUs 1-2) → `group_fully_busy`**: Create 2 CFS tasks, pin them to CPUs 1-2, add to cgroup `t1` (unrestricted bandwidth). This fills the local group exactly, making it fully busy.
- **Busiest group (CPUs 3-4) → `group_overloaded`**: Create 4+ CFS tasks, pin them to CPUs 3-4, add some to cgroup `t0` (bandwidth-throttled). Having more tasks than CPUs in this group makes it overloaded. The bandwidth throttling on some tasks lowers their effective load contribution to `sds->total_load`, which reduces `sds->avg_load`.

```c
// Local group: 2 high-load unrestricted tasks on CPUs 1-2
struct task_struct *local_tasks[2];
for (int i = 0; i < 2; i++) {
    local_tasks[i] = kstep_task_create();
    kstep_task_pin(local_tasks[i], 1, 2);
    kstep_cgroup_add_task("t1", local_tasks[i]->pid);
    kstep_task_wakeup(local_tasks[i]);
}

// Busiest group: 5+ tasks on CPUs 3-4, some bandwidth-throttled
struct task_struct *busy_tasks[6];
for (int i = 0; i < 6; i++) {
    busy_tasks[i] = kstep_task_create();
    kstep_task_pin(busy_tasks[i], 3, 4);
    if (i < 4)
        kstep_cgroup_add_task("t0", busy_tasks[i]->pid);  // throttled
    else
        kstep_cgroup_add_task("t1", busy_tasks[i]->pid);  // unrestricted
    kstep_task_wakeup(busy_tasks[i]);
}
```

### Step 4: Drive Ticks and Observe

Run sufficient ticks for the bandwidth throttling to take effect and for load balancing to be triggered:

```c
kstep_tick_repeat(2000);
```

### Step 5: Instrumenting the Bug Detection

Use the `on_sched_balance_begin` or `on_sched_balance_selected` callback to inspect the internal state of `calculate_imbalance()`. Alternatively, use `KSYM_IMPORT` to access relevant kernel symbols and inspect `struct lb_env`, `struct sd_lb_stats`, and `struct sg_lb_stats` values.

The primary detection approach is to hook into the load balancing path and check whether `env->imbalance` has an unreasonably large value. On the buggy kernel, when `local->avg_load > sds->avg_load`, `env->imbalance` will be a very large number (due to unsigned underflow). On the fixed kernel, `env->imbalance` will be 0 in the same scenario.

A practical approach:
1. In `on_sched_balance_selected`, read the `env->imbalance` value after `calculate_imbalance()` completes (by inspecting the resulting migration state).
2. Alternatively, add a custom `printk()` inside the kernel's `calculate_imbalance()` function via a small patch that logs `local->avg_load`, `sds->avg_load`, and `env->imbalance` when the fully_busy path is taken.
3. If direct instrumentation is difficult, observe the side effect: after sufficient ticks, check whether tasks have been migrated from the overloaded group to the already-above-average local group. On the buggy kernel, tasks will be migrated (increasing local load above average). On the fixed kernel, no migration occurs when local is above average.

### Step 6: Pass/Fail Criteria

- **Buggy kernel**: After load balancing runs, observe that `env->imbalance` is a very large value (e.g., > 100 when loads are in the range 100-300), OR observe that tasks were migrated from the busiest group to the local group even though the local group's average load exceeded the system-wide average. The migration makes the local group even more loaded. Use `kstep_fail()` if the imbalance exceeds a reasonable threshold or if the local group ends up with more tasks than expected.
- **Fixed kernel**: `env->imbalance` is 0 when `local->avg_load >= sds->avg_load`. No migration occurs in this scenario. The task distribution remains stable. Use `kstep_pass()`.

### Step 7: kSTEP Extensions Needed

The main challenge is observing the internal `calculate_imbalance()` state from the driver. The existing `on_sched_balance_selected` callback provides the CPU and sched_domain, but not the `lb_env` or `sd_lb_stats` structures. Two approaches:

1. **Indirect observation (no kSTEP changes)**: Instead of reading internal variables directly, detect the bug by its observable effect — inappropriate task migration. Before and after load balancing ticks, count the number of running tasks on each group's CPUs. On the buggy kernel, tasks will migrate from the overloaded group to the fully_busy group (increasing imbalance). On the fixed kernel, no such migration occurs. This can be done purely with existing kSTEP APIs by reading `cpu_rq(cpu)->nr_running` or `cpu_rq(cpu)->cfs.nr_running` via `internal.h`.

2. **Direct observation (minor kSTEP extension)**: Add a new callback `on_calculate_imbalance` that fires after `calculate_imbalance()` returns, exposing `env->imbalance`, `env->migration_type`, and the `sd_lb_stats` values. This would provide precise bug detection but requires modifying kSTEP's hook infrastructure.

The indirect approach (option 1) is sufficient and requires no kSTEP modifications. The driver should track `nr_running` per CPU across ticks and flag if a task migrates to the already-above-average group. Expected behavior on the buggy kernel: tasks on CPUs 3-4 migrate to CPUs 1-2 even though CPUs 1-2 are already fully busy and above system average. Expected behavior on the fixed kernel: no such migration.
