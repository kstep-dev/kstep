# PELT: load_avg and load_sum Desynchronization During Cgroup Load Propagation

**Commit:** `7c7ad626d9a0ff0a36c1e2a3cfbbc6a13828d5eb`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.13-rc6
**Buggy since:** v4.15-rc1 (introduced by commit `0e2d2aaaae52` "sched/fair: Rewrite PELT migration propagation")

## Bug Description

The Per-Entity Load Tracking (PELT) subsystem maintains two related load metrics for each scheduling entity and CFS run queue: `load_avg` (the averaged load value) and `load_sum` (the accumulated sum from which the average is derived). The invariant relationship between these two values is: `load_avg = load_sum / divider`, where `divider = PELT_MIN_DIVIDER + period_contrib`. Keeping these two values synchronized is critical for correct scheduling decisions, load balancing, and cgroup hierarchy management.

When tasks migrate between CPUs or cgroups, the PELT values must be propagated up the cgroup hierarchy. The function `update_tg_cfs_load()` is responsible for updating the load tracking values of a group scheduling entity (`se`) and its parent CFS run queue (`cfs_rq`) based on changes in the child group CFS run queue (`gcfs_rq`). In the buggy code, this function updates `cfs_rq->avg.load_sum` using an additive delta computed independently from `cfs_rq->avg.load_avg`, which can cause the two values to fall out of sync due to the `add_positive()` clamping behavior.

The desynchronization manifests when a CFS run queue has `load_avg > 0` but `load_sum == 0`. This is problematic because the function `cfs_rq_is_decayed()`, which decides whether a CFS run queue should be removed from the leaf list, checks `load_sum` (not `load_avg`) to determine if the run queue has fully decayed. When `load_sum` is zero but `load_avg` is not, the CFS run queue is prematurely removed from the leaf list while it still contributes load to the task group's `tg->load_avg`, resulting in a stale `tg_load_avg_contrib` value that never gets cleaned up.

## Root Cause

The root cause lies in the `update_tg_cfs_load()` function, specifically in how it updates `cfs_rq->avg.load_sum` relative to `cfs_rq->avg.load_avg`. In the buggy code:

```c
delta_sum = load_sum - (s64)se_weight(se) * se->avg.load_sum;
delta_avg = load_avg - se->avg.load_avg;

se->avg.load_sum = runnable_sum;
se->avg.load_avg = load_avg;
add_positive(&cfs_rq->avg.load_avg, delta_avg);
add_positive(&cfs_rq->avg.load_sum, delta_sum);
```

The `delta_sum` and `delta_avg` are computed independently. `delta_sum` is weighted by `se_weight(se)` (the scheduling entity's weight, typically 1024 for nice-0), making it operate in a different numerical range than `delta_avg`. The `add_positive()` macro clamps the result to zero if the addition would produce a negative value:

```c
#define add_positive(_ptr, _val) do {
    ...
    res = var + val;
    if (val < 0 && res > var)
        res = 0;
    WRITE_ONCE(*ptr, res);
} while (0)
```

The critical problem is that `delta_sum` and `delta_avg` can produce different clamping behavior. When both `load_sum` and `load_avg` are being reduced (negative deltas), the `load_sum` delta is weighted by `se_weight(se)`, making it a much larger magnitude negative number. This can cause `cfs_rq->avg.load_sum` to be clamped to zero by `add_positive()` while `cfs_rq->avg.load_avg` is only partially reduced (not clamped to zero), because `delta_avg` is a smaller magnitude negative number.

Consider a concrete scenario: if `cfs_rq->avg.load_avg` is small but nonzero (e.g., 2), and `delta_avg` is -1, then `load_avg` becomes 1. But `cfs_rq->avg.load_sum` might be, say, 500, and `delta_sum` could be -600 (because it's weighted by `se_weight(se) = 1024`). The `add_positive()` macro clamps `load_sum` to 0 because 500 + (-600) would be negative. Now `load_sum == 0` but `load_avg == 1`.

This desynchronization is avoided in `update_tg_cfs_util()` and `update_tg_cfs_runnable()`, which correctly recompute `_sum` from `_avg` using the formula `_sum = _avg * divider`, ensuring the invariant is maintained. The load propagation path was the only one that used the additive delta approach.

## Consequence

The primary consequence is a fairness problem between cgroups. When `load_sum` becomes zero while `load_avg` is still nonzero, the `cfs_rq_is_decayed()` function returns true (at the buggy kernel version, it only checks `load_sum`, `util_sum`, and `runnable_sum`), causing the CFS run queue to be removed from the leaf CFS run queue list via `list_del_leaf_cfs_rq()`. Once removed from the leaf list, the CFS run queue is no longer visited during `__update_blocked_fair()`, which means:

1. `update_tg_load_avg()` is never called for this CFS run queue again, so its `cfs_rq->tg_load_avg_contrib` value becomes stale and is never subtracted from `tg->load_avg`.
2. The stale contribution inflates `tg->load_avg` for the task group, which affects the weight calculation for all group scheduling entities: `ge->weight = tg->weight * grq->avg.load_avg / tg->load_avg`. An inflated `tg->load_avg` makes the denominator larger, reducing the effective weight of all group entities, leading to unfair CPU time distribution.

Odin Ugedal reported this as observable unfairness between cgroups in production. The reproduction showed that after tasks migrate between cgroups with bandwidth throttling, `tg_load_avg_contrib` values become stale. For example, a cgroup's `tg_load_avg_contrib` might read 1040 while `tg_load_avg` is 2062, indicating a large stale phantom contribution from a decayed CFS run queue that was prematurely removed from the leaf list. This causes one cgroup to receive disproportionately less CPU time than another cgroup with the same weight, violating the fairness guarantees of CFS.

The issue is not a crash or a hang but a silent correctness problem: the scheduler makes suboptimal weight calculations, which can persist until the cgroup hierarchy is torn down or the stale values are coincidentally corrected by subsequent task migrations.

## Fix Summary

The fix changes `update_tg_cfs_load()` to recompute `cfs_rq->avg.load_sum` directly from `cfs_rq->avg.load_avg` using the PELT divider, rather than applying an independent additive delta. The key change is:

```c
// Before (buggy):
add_positive(&cfs_rq->avg.load_avg, delta_avg);
add_positive(&cfs_rq->avg.load_sum, delta_sum);

// After (fixed):
add_positive(&cfs_rq->avg.load_avg, delta);
cfs_rq->avg.load_sum = cfs_rq->avg.load_avg * divider;
```

This ensures that `load_sum` and `load_avg` are always synchronized. When `load_avg` is clamped to zero by `add_positive()`, `load_sum` will also be zero (0 * divider = 0). When `load_avg` is nonzero, `load_sum` will be the correctly corresponding value. This matches the approach already used for `util_sum` and `runnable_sum` in the sibling functions `update_tg_cfs_util()` and `update_tg_cfs_runnable()`.

The fix also removes the now-unused `delta_sum` variable and renames `delta_avg` to just `delta` for clarity. Peter Zijlstra's review suggested these cleanups, which Vincent Guittot incorporated into the final committed version. The fix is correct because the PELT invariant (`_avg = _sum / divider`, equivalently `_sum = _avg * divider`) is the authoritative relationship, and recomputing `_sum` from `_avg` after each update guarantees the invariant holds, eliminating the possibility of the desynchronization that triggers the bug.

## Triggering Conditions

The bug requires the following conditions to trigger:

- **CONFIG_FAIR_GROUP_SCHED must be enabled**: The `update_tg_cfs_load()` function is only compiled and called when cgroup-based fair scheduling is active. Without cgroups, the propagation path doesn't exist.

- **A cgroup hierarchy with at least two levels**: There must be a parent cgroup containing child cgroups (task groups) so that the propagation path from child CFS run queue to parent CFS run queue is exercised.

- **Task migration or load change that triggers propagation with a negative delta**: A task must migrate away from a CPU (or have its load decay) such that `prop_runnable_sum` is negative, causing `update_tg_cfs_load()` to reduce the load values. The negative delta is where the desynchronization occurs due to `add_positive()` clamping.

- **The cfs_rq's load_avg must be small but nonzero, and delta_sum must be large enough to clamp load_sum to zero**: This happens when the CFS run queue has a small residual load (e.g., from recently migrated or sleeping tasks), and the weighted delta (`se_weight * delta`) is large enough to overshoot load_sum to negative, triggering the clamp.

- **The __update_blocked_fair() periodic check must run after the desynchronization**: The leaf list removal happens during the periodic blocked load update, which iterates over all leaf CFS run queues and removes decayed ones.

The bug is more likely to trigger when: (a) CPU bandwidth throttling is used (`cpu.max` or `cpu.cfs_quota_us`), because throttling causes tasks to be blocked and then unthrottled, creating significant load transients that produce large deltas; (b) tasks are migrated between CPUs assigned to different cgroups, especially via `cpuset.cpus` changes; (c) the system has many cgroups with varying loads, increasing the frequency of propagation events.

The reproduction script from Odin Ugedal's report uses `stress --cpu 1` tasks in nested cgroups with `cpu.max` throttling and `cpuset.cpus` changes to force task migrations. The key sequence is: start CPU-intensive tasks in throttled cgroups on different CPUs, then change the cpuset to force migration to a shared CPU, then remove throttling. This creates the conditions where load_sum can be clamped to zero while load_avg remains nonzero during propagation.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by creating a cgroup hierarchy and triggering load propagation with negative deltas that cause the load_sum/load_avg desynchronization. Here is the step-by-step plan:

**1. Topology and CPU setup:**
Configure QEMU with at least 2 CPUs (e.g., 2 or 4 CPUs). No special topology (SMT/cluster) is required. Use `kstep_topo_init()` and `kstep_topo_apply()` for basic setup.

**2. Cgroup hierarchy creation:**
Create a nested cgroup hierarchy to enable the propagation path:
```
/slice          (parent cgroup)
/slice/cg1      (child cgroup 1)
/slice/cg2      (child cgroup 2)
```
Use `kstep_cgroup_create("slice")`, `kstep_cgroup_create("slice/cg1")`, `kstep_cgroup_create("slice/cg2")`.

**3. Set CPU bandwidth throttling:**
Set bandwidth quota on the parent cgroup to increase the chance of load transients: `kstep_sysctl_write()` or use the cgroup API to set `cpu.max` equivalent. If kSTEP's `kstep_cgroup_set_weight()` is insufficient, a kSTEP extension to write cgroup bandwidth settings (e.g., `kstep_cgroup_set_quota(name, quota_us, period_us)`) may be needed.

**4. Task creation and placement:**
Create 2 CFS tasks:
- Task A: pinned to CPU 1, added to `slice/cg1` via `kstep_cgroup_add_task("slice/cg1", pid_a)`.
- Task B: pinned to CPU 2 (or CPU 3), added to `slice/cg2` via `kstep_cgroup_add_task("slice/cg2", pid_b)`.
Use `kstep_task_create()` to create the tasks, `kstep_task_pin()` for CPU affinity, and make them CFS tasks.

**5. Build up load:**
Wake up both tasks and let them run for many ticks to build up PELT load averages: `kstep_task_wakeup(task_a)`, `kstep_task_wakeup(task_b)`, then `kstep_tick_repeat(500)` or similar to accumulate load.

**6. Trigger migration (change CPU affinity):**
Change task A's affinity to move it to the same CPU as task B. This triggers PELT propagation with a negative delta on the source CPU's cgroup CFS run queue (the load is being removed from that CPU): `kstep_task_pin(task_a, cpu2, cpu2)`.

**7. Tick to trigger blocked load update:**
Run several ticks to trigger `__update_blocked_fair()`, which will iterate over the leaf CFS run queues: `kstep_tick_repeat(100)`.

**8. Detection — check for desynchronization:**
Use `KSYM_IMPORT` to access internal PELT values. Import `cpu_rq` and read the CFS run queue's `avg.load_sum` and `avg.load_avg` for the cgroup's CFS run queues on the source CPU. The detection criteria are:

- **Bug present (buggy kernel):** After propagation, observe `cfs_rq->avg.load_sum == 0` while `cfs_rq->avg.load_avg > 0` for the parent cgroup's CFS run queue on the source CPU. Additionally, check that the CFS run queue has been removed from the leaf list (`cfs_rq->on_list == 0`) while `cfs_rq->tg_load_avg_contrib > 0`, indicating a stale contribution.

- **Bug fixed (fixed kernel):** `load_sum` and `load_avg` remain synchronized: when `load_avg == 0`, `load_sum == 0`; when `load_avg > 0`, `load_sum == load_avg * divider`.

**9. Alternative detection via tg->load_avg:**
Another approach is to observe the fairness impact. After the bug triggers, read `tg->load_avg` for the affected task group and compare it to the sum of all `cfs_rq->tg_load_avg_contrib`. On the buggy kernel, `tg->load_avg` will be higher than expected (stale contribution not cleaned up). On the fixed kernel, they will match.

**10. Detailed driver structure:**
```c
// In on_tick_begin or a custom check function:
struct cfs_rq *cfs_rq = ...; // get parent cgroup's cfs_rq on source CPU
u32 divider = get_pelt_divider(&cfs_rq->avg);
if (cfs_rq->avg.load_avg > 0 && cfs_rq->avg.load_sum == 0) {
    kstep_fail("load_avg=%lu but load_sum=0 — desynchronized!",
               cfs_rq->avg.load_avg);
} else {
    kstep_pass("load_avg and load_sum are synchronized");
}
```

**11. Possible kSTEP extensions needed:**
- If kSTEP does not currently support setting cgroup CPU bandwidth (quota/period), a `kstep_cgroup_set_quota(name, quota_us, period_us)` helper would be useful. However, the bug may be triggerable without bandwidth throttling — pure task migration that causes negative propagation deltas should suffice, since the core issue is in `update_tg_cfs_load()` which runs on any propagation event.
- The `kstep_cgroup_set_cpuset()` function is already available and can be used to change CPU assignments for cgroups, which triggers the migration and propagation path.
- Access to internal CFS run queue structures is available through `kmod/driver.h` and `internal.h` (via `cpu_rq(cpu)`, `cfs_rq`, etc.), so reading `load_avg`, `load_sum`, `on_list`, and `tg_load_avg_contrib` is straightforward.

**12. Expected behavior summary:**
- **Buggy kernel:** After task migration from cg1's source CPU, the parent cgroup's CFS run queue on that CPU shows `load_sum == 0` while `load_avg > 0`. The CFS run queue gets removed from the leaf list, and `tg_load_avg_contrib` becomes stale.
- **Fixed kernel:** After the same migration, `load_sum = load_avg * divider` always holds. The CFS run queue is only removed from the leaf list when truly fully decayed (both `load_avg` and `load_sum` are zero).

**13. Reliability considerations:**
The bug requires specific numerical conditions (load_sum being clamped to zero while load_avg is not), which depend on the exact PELT values at the time of propagation. Running many migration cycles and checking after each one increases the chance of catching the desynchronization. The driver should loop through multiple migration/tick/check cycles. Using `on_sched_balance_begin` or `on_sched_softirq_end` callbacks to inspect state immediately after the blocked load update would give the most precise detection window.
