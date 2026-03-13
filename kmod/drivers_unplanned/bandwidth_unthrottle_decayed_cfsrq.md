# Bandwidth: Missing Decayed cfs_rq on Leaf List After Unthrottle

**Commit:** `a7b359fc6a37faaf472125867c8dc5a068c90982`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.13-rc7
**Buggy since:** v5.1-rc1 (commit `31bc6aeaab1d` — "sched/fair: Optimize update_blocked_averages()")

## Bug Description

The Linux CFS scheduler maintains a per-runqueue "leaf list" (`rq->leaf_cfs_rq_list`) that tracks which `cfs_rq` structures need their PELT (Per-Entity Load Tracking) averages updated during `update_blocked_averages()`. When CFS bandwidth throttling is enabled, throttled `cfs_rq` nodes and their descendants are removed from this leaf list (via `tg_throttle_down()` → `list_del_leaf_cfs_rq()`). When a cfs_rq is later unthrottled, descendants must be re-added to the leaf list so their blocked load averages can continue to decay properly.

The bug is in the `tg_unthrottle_up()` function, which is called via `walk_tg_tree_from()` during unthrottling. The original code (introduced by commit `31bc6aeaab1d`) only re-added a descendant `cfs_rq` to the leaf list if it had one or more running entities (`cfs_rq->nr_running >= 1`). This heuristic was too narrow: a `cfs_rq` can have zero running entities but still carry non-zero PELT load averages (`load_sum`, `util_sum`, `runnable_sum`) or non-zero `cfs_rq->load.weight` from recently dequeued entities. Such `cfs_rq`s need to remain on the leaf list so their averages decay to zero over time. If they are excluded, their stale load values persist indefinitely, corrupting the load calculations for the parent task group.

The practical consequence is severe fairness degradation. For two sibling cgroups with equal weight, each running one CPU-intensive task, the expected CPU share should be approximately 50/50. Instead, due to the stale undecayed load, one cgroup can receive ~99% of CPU time while the other gets only ~1%. The bug was observed and reported with the `stress` tool, showing CPU utilization of 88.8% vs 3.0% for equally weighted cgroups.

The issue is most likely to manifest when CFS bandwidth throttling interacts with group scheduling in a hierarchy where some descendant `cfs_rq`s temporarily have no running tasks but still carry undecayed PELT load from recently blocked or migrated entities.

## Root Cause

The root cause lies in the `tg_unthrottle_up()` function in `kernel/sched/fair.c`. When a cfs_rq's `throttle_count` reaches zero (meaning the cfs_rq is becoming fully unthrottled), the function decides whether to re-add this `cfs_rq` to the leaf list. The buggy condition was:

```c
/* Add cfs_rq with already running entity in the list */
if (cfs_rq->nr_running >= 1)
    list_add_leaf_cfs_rq(cfs_rq);
```

This condition only checks whether there are currently runnable entities in the cfs_rq. However, during throttling, the PELT averaging machinery is frozen for the throttled subtree — `cfs_rq->avg.load_sum`, `cfs_rq->avg.util_sum`, and `cfs_rq->avg.runnable_sum` retain whatever values they had at throttle time. When the cfs_rq is unthrottled, if `nr_running == 0` but the PELT sums are non-zero, the cfs_rq is not added back to the leaf list.

The consequence of not being on the leaf list is that `__update_blocked_fair()` never iterates over this cfs_rq. `__update_blocked_fair()` is the function called periodically (via `update_blocked_averages()`) to decay the PELT values of all cfs_rqs on the leaf list. The bottom of this function contains:

```c
if (cfs_rq_is_decayed(cfs_rq))
    list_del_leaf_cfs_rq(cfs_rq);
```

So normally, a cfs_rq stays on the leaf list until its PELT values fully decay to zero. But if it never gets added back to the leaf list after unthrottling, its stale PELT values never decay.

These stale PELT values propagate up the hierarchy through `update_tg_load_avg()`, which contributes the cfs_rq's `avg.load_avg` to the task group's global `tg->load_avg` via `tg_load_avg_contrib`. Since `tg->load_avg` is used by `update_cfs_group()` to calculate the `se->load.weight` of the group's scheduling entity on its parent runqueue, a stale (too high) load average for one group's cfs_rq causes the scheduler to assign an incorrect weight, leading to one group receiving a disproportionate share of CPU time relative to its sibling groups.

The specific failure scenario is: (1) Two sibling cgroups A and B with equal weight, each running one CPU-bound task. (2) Cgroup A or an ancestor gets throttled by CFS bandwidth. (3) During throttling, descendant cfs_rqs are removed from the leaf list via `tg_throttle_down()`. (4) Some descendant cfs_rqs have tasks that block during the throttled period, leaving `nr_running == 0` but non-zero PELT sums. (5) When unthrottled, `tg_unthrottle_up()` skips these cfs_rqs because `nr_running == 0`. (6) The stale PELT values inflate one group's load, causing the scheduler to give it less CPU time than its fair share.

## Consequence

The primary observable impact is severe CPU fairness violation between cgroup siblings. For equally weighted cgroups each running one CPU-bound task, the expected ~50/50 CPU split degrades to ratios as extreme as 99/1 or 88/3. This is not a crash or data corruption bug, but it fundamentally breaks the scheduling fairness guarantees that cgroup CPU controllers are designed to provide.

In production environments, this means that containerized or cgroup-controlled workloads can experience starvation — one container or service gets almost no CPU time despite having equal (or higher) configured CPU weight relative to its siblings. This is particularly dangerous because it appears non-deterministic (it depends on the timing of throttle/unthrottle events relative to task sleeping patterns), making it difficult to diagnose. Users may observe that a service suddenly gets dramatically less CPU than expected, with no obvious cause.

The bug affects any system using CFS bandwidth throttling (`cpu.max` in cgroup v2, or `cpu.cfs_quota_us`/`cpu.cfs_period_us` in cgroup v1) in combination with hierarchical cgroup scheduling. This is an extremely common configuration in container orchestration systems (Kubernetes, Docker, systemd slices). The bug persisted for over two years in the kernel (v5.1 through v5.13-rc6), affecting all LTS kernels in that range unless backported.

## Fix Summary

The fix modifies the condition in `tg_unthrottle_up()` to also re-add cfs_rqs that have non-zero (undecayed) PELT values, not just those with running entities. The new condition is:

```c
/* Add cfs_rq with load or one or more already running entities to the list */
if (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running)
    list_add_leaf_cfs_rq(cfs_rq);
```

The `cfs_rq_is_decayed()` function checks whether the cfs_rq's `load.weight`, `avg.load_sum`, `avg.util_sum`, and `avg.runnable_sum` are all zero. If any of these are non-zero, the cfs_rq is not yet fully decayed and must be on the leaf list so `__update_blocked_fair()` can continue decaying its PELT values.

Additionally, the fix moves the `cfs_rq_is_decayed()` function definition from its original location (just before `__update_blocked_fair()`) to an earlier position in the file (just after the `#ifdef CONFIG_SMP` / `#ifdef CONFIG_FAIR_GROUP_SCHED` guards), so it can be used by `tg_unthrottle_up()` which appears earlier in the source. A stub returning `true` is provided for the `!CONFIG_SMP` case, since PELT is only used with SMP.

This fix is correct because it ensures that after unthrottling, every cfs_rq that still has non-zero load state is placed back on the leaf list, where `__update_blocked_fair()` will naturally decay its PELT values and eventually remove it once fully decayed (`cfs_rq_is_decayed()` returns true). This restores the invariant that was implicitly assumed by the `__update_blocked_fair()` cleanup logic.

## Triggering Conditions

The following conditions must all be met to trigger the bug:

1. **CFS bandwidth throttling enabled**: At least one cgroup in the hierarchy must have a CPU bandwidth limit configured (e.g., `cpu.max` or `cpu.cfs_quota_us`). The limit must be low enough to cause actual throttling.

2. **Hierarchical cgroup structure**: There must be a cgroup hierarchy with at least two levels — a parent with a bandwidth limit, and child cgroups (siblings) with tasks. The bug manifests in the children or descendants of the throttled cgroup.

3. **Tasks that block during throttle period**: The key trigger is having a task that is running in a descendant cfs_rq, contributes to PELT load, and then blocks (stops running) while the cfs_rq is throttled. This leaves `nr_running == 0` but non-zero PELT values. The blocking can happen naturally due to I/O, sleep, or being descheduled.

4. **SMP configuration**: The bug only affects `CONFIG_SMP` kernels, as PELT is only active with SMP. On uniprocessor builds, `cfs_rq_is_decayed()` trivially returns `true`.

5. **Kernel version**: v5.1 through v5.13-rc6. The bug was introduced by commit `31bc6aeaab1d` (merged for v5.1-rc1) and fixed by this commit in v5.13-rc7.

6. **Equal or similar cgroup weights**: The fairness impact is most visible when sibling cgroups have equal weights and each runs a CPU-bound workload. The stale load on one cfs_rq causes the scheduler's weight calculations to skew, giving one group significantly more CPU than the other.

The bug is reliably reproducible by: creating two sibling cgroups with bandwidth limits, running a CPU-bound task in each, and observing their CPU time over several seconds. The `stress --cpu 1` command in each cgroup is sufficient. The stale PELT values accumulate over multiple throttle/unthrottle cycles, causing the imbalance to grow progressively worse.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why This Bug Cannot Be Reproduced with kSTEP

**The kernel version is too old.** kSTEP supports Linux v5.15 and newer only. This bug was fixed in v5.13-rc7, meaning the fix is already present in all kernels v5.15 and later. The buggy code (with the `cfs_rq->nr_running >= 1` condition in `tg_unthrottle_up()`) does not exist in any kernel version that kSTEP can run. To reproduce this bug, one would need to check out a kernel between v5.1 and v5.13-rc6, which is outside kSTEP's supported range.

### 2. What Would Need to Be Added to kSTEP

If kSTEP supported older kernels (v5.1–v5.13), no fundamental architectural changes would be needed. The existing kSTEP infrastructure already supports:
- Creating cgroups with `kstep_cgroup_create()`
- Setting bandwidth limits with `kstep_cgroup_write(name, "cpu.max", ...)`
- Creating and managing tasks with `kstep_task_create()`, `kstep_task_block()`, `kstep_task_wakeup()`
- Driving the CFS bandwidth period timer via `kstep_cfs_bandwidth_tick()` in `tick.c`
- Accessing internal kernel state via `KSYM_IMPORT()` and `cpu_rq()` to inspect cfs_rq fields

The only change needed would be extending kernel version support to cover v5.1–v5.13.

### 3. Version Constraint

The fix landed in v5.13-rc7 (commit `a7b359fc6a37faaf472125867c8dc5a068c90982`). kSTEP's minimum supported version is v5.15. Therefore, the bug is already fixed in every kernel kSTEP can run against. This is the sole reason for classifying this bug as unplanned.

### 4. Alternative Reproduction Methods Outside kSTEP

The bug can be reproduced on a real or virtual machine running a kernel between v5.1 and v5.13-rc6 with the following steps:

1. Enable cgroup v2 with CPU controller (`cgroup_no_v1=all` boot parameter, or use a distribution default).
2. Create a cgroup hierarchy:
   ```bash
   mkdir -p /sys/fs/cgroup/test
   echo "+cpu" > /sys/fs/cgroup/test/cgroup.subtree_control
   mkdir /sys/fs/cgroup/test/A /sys/fs/cgroup/test/B
   ```
3. Set a bandwidth limit on the parent or one of the children:
   ```bash
   echo "50000 100000" > /sys/fs/cgroup/test/cpu.max  # 50% bandwidth
   ```
4. Run CPU-bound tasks in each child cgroup:
   ```bash
   cgexec -g cpu:test/A stress --cpu 1 &
   cgexec -g cpu:test/B stress --cpu 1 &
   ```
5. After several seconds, observe CPU time:
   ```bash
   ps u -C stress
   ```
   On a buggy kernel, one task will show ~90%+ CPU while the other shows ~3-10%, despite equal cgroup weights. On a fixed kernel, both tasks will show approximately equal CPU usage (~50% each, adjusted for the bandwidth limit).

Alternatively, the LTP (Linux Test Project) test suite includes `cfs_bandwidth01` which can exercise these code paths, though the original reporter used simple `stress` commands.
