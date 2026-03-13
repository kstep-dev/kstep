# Bandwidth: Decayed Parent cfs_rq Skipped on Leaf List During Unthrottle

**Commit:** `fdaba61ef8a268d4136d0a113d153f7a89eb9984`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.13
**Buggy since:** v5.13-rc7 (introduced by `a7b359fc6a37` "sched/fair: Correctly insert cfs_rq's to list on unthrottle")

## Bug Description

When CFS bandwidth throttling is used with nested cgroup hierarchies, intermediate `cfs_rq` nodes can be incorrectly omitted from the `rq->leaf_cfs_rq_list` after an unthrottle operation. This list is critical for the scheduler's PELT (Per-Entity Load Tracking) update path: `__update_blocked_fair()` iterates it to decay load averages and garbage-collect fully decayed cfs_rqs. The invariant is that if a child `cfs_rq` is on the leaf list, all its ancestor `cfs_rq`s up to the root must also be on the list, because the list is iterated bottom-up and parent updates depend on child updates having already occurred.

The bug arises from an interaction between two independent events: (1) a child `cfs_rq` whose load fully decays while throttled, and (2) a parent `cfs_rq` that is also throttled. When both are unthrottled via `tg_unthrottle_up()`, the parent's `cfs_rq` may be considered "fully decayed" (`cfs_rq_is_decayed()` returns true) because it has no load, no `nr_running` tasks, and zero PELT sums. However, a child `cfs_rq` that still has running tasks (or non-decayed load) has already been added to the leaf list. This creates a broken list state: a child is present but its parent is absent.

This was reported while running the LTP `cfs_bandwidth01` test, which exercises CFS bandwidth limiting with nested cgroup hierarchies. The specific reproducer involved a 3-level-deep cgroup hierarchy where `worker3` is inside `level3b`, which is inside `level2`. The timeline was: worker3 gets throttled, level3b's load decays to zero (no more tasks running), level2 gets throttled, then worker3 is unthrottled, then level2 is unthrottled. At this point, worker3's `cfs_rq` is added to the leaf list but level3b's `cfs_rq` is not (because it appears fully decayed with `nr_running==0`), breaking the parent-before-child invariant.

The consequence is a warning triggered by `assert_list_leaf_cfs_rq()` which checks that `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list`. When the intermediate cfs_rq (level3b) is missing, the `tmp_alone_branch` pointer is left dangling at a branch endpoint instead of being properly reset to the list head, triggering this assertion.

## Root Cause

The root cause lies in `tg_unthrottle_up()`, which is called via `walk_tg_tree_from()` during `unthrottle_cfs_rq()`. For each `cfs_rq` being unthrottled, `tg_unthrottle_up()` decides whether to re-add it to the leaf list:

```c
static int tg_unthrottle_up(struct task_group *tg, void *data)
{
    struct rq *rq = data;
    struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

    cfs_rq->throttle_count--;
    if (!cfs_rq->throttle_count) {
        /* Add cfs_rq with load or one or more already running entities to the list */
        if (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running)
            list_add_leaf_cfs_rq(cfs_rq);
    }
    return 0;
}
```

The condition `!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running` was introduced by commit `a7b359fc6a37` to be smarter about which `cfs_rq`s to re-add (previously only `nr_running >= 1` was checked). The `cfs_rq_is_decayed()` function checks whether `load.weight`, `avg.load_sum`, `avg.util_sum`, and `avg.runnable_sum` are all zero.

The problem is that this check does not consider whether a **child** `cfs_rq` is already on (or will be on) the leaf list. Consider the cgroup hierarchy:

```
level2
  └── level3b
       └── worker3
```

When `walk_tg_tree_from()` calls `tg_unthrottle_up()` bottom-up:
1. **worker3's cfs_rq** has running tasks, so it's added to the leaf list via `list_add_leaf_cfs_rq()`.
2. **level3b's cfs_rq** has `nr_running == 0` (no direct entities) and is fully decayed (all PELT sums are zero because it decayed while worker3 was throttled). So `!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running` evaluates to `false`, and it is **not** added to the list.
3. **level2's cfs_rq** has entities and gets added.

This leaves worker3's `cfs_rq` on the list with its parent (level3b) missing. The `list_add_leaf_cfs_rq()` function maintains the invariant by using `rq->tmp_alone_branch`: when a child is added without a parent already on the list, it sets `tmp_alone_branch` to point to the child, expecting the parent to be added later and connect the branch. When level3b is skipped, the branch is never connected, and `tmp_alone_branch` remains pointing into the middle of the list instead of being reset to `&rq->leaf_cfs_rq_list`.

The `assert_list_leaf_cfs_rq()` call at the end of `unthrottle_cfs_rq()` then fires because `rq->tmp_alone_branch != &rq->leaf_cfs_rq_list`.

## Consequence

The immediate observable consequence is a `SCHED_WARN_ON` warning in the kernel log triggered by `assert_list_leaf_cfs_rq()`:

```
SCHED_WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list)
```

This warning fires from `unthrottle_cfs_rq()` after the leaf list repair loop fails to fully connect the branch.

Beyond the warning, the missing intermediate `cfs_rq` on the leaf list means `__update_blocked_fair()` will not iterate over it. This means:
- The PELT load averages for that intermediate cfs_rq will not be updated/decayed properly, potentially leading to stale load information.
- The `update_tg_load_avg()` call for that task group will be skipped, causing the task group's global `load_avg` to be incorrect.
- Since `list_add_leaf_cfs_rq()` adds children before parents to ensure bottom-up iteration order, a missing intermediate node means the parent's load propagation from its children may be incomplete.

In practice, this manifests as unfair CPU distribution between sibling control groups. As documented in the parent bug (`a7b359fc6a37`), this class of leaf-list corruption can lead to a load ratio of 99/1 between equally weighted sibling cgroups, instead of the expected 50/50 split. The severity depends on workload: systems with deep cgroup hierarchies, CFS bandwidth limits, and variable task activity patterns are most susceptible.

## Fix Summary

The fix introduces a new helper function `child_cfs_rq_on_list()` and adds a check to `cfs_rq_is_decayed()` that prevents a `cfs_rq` from being considered "fully decayed" if one of its children is present (or pending) on the leaf list.

```c
static inline bool child_cfs_rq_on_list(struct cfs_rq *cfs_rq)
{
    struct cfs_rq *prev_cfs_rq;
    struct list_head *prev;

    if (cfs_rq->on_list) {
        prev = cfs_rq->leaf_cfs_rq_list.prev;
    } else {
        struct rq *rq = rq_of(cfs_rq);
        prev = rq->tmp_alone_branch;
    }

    prev_cfs_rq = container_of(prev, struct cfs_rq, leaf_cfs_rq_list);
    return (prev_cfs_rq->tg->parent == cfs_rq->tg);
}
```

This function exploits the list ordering invariant: `list_add_leaf_cfs_rq()` always places child `cfs_rq`s immediately before their parents on the list. So checking the previous entry (`prev`) on the list tells us if a child is present. If the `cfs_rq` is not yet on the list (`!on_list`), it checks `rq->tmp_alone_branch`, which points to the most recently added child that hasn't been connected to a parent yet.

The check is then added to `cfs_rq_is_decayed()`:
```c
if (child_cfs_rq_on_list(cfs_rq))
    return false;
```

This ensures that even if a `cfs_rq` has zero load, zero PELT sums, and zero `nr_running`, it will still be considered "not decayed" (and thus re-added to the leaf list) if a child `cfs_rq` needs it as an ancestor on the list. This preserves the list invariant that every child on the list has all its ancestors also on the list.

The fix is correct because it addresses the root cause (the decayed check ignoring child presence) rather than applying a workaround. It also benefits `__update_blocked_fair()`, which uses the same `cfs_rq_is_decayed()` to garbage-collect decayed cfs_rqs from the leaf list—this check prevents premature removal of intermediate nodes that still have active descendants.

## Triggering Conditions

To trigger this bug, the following conditions must hold simultaneously:

1. **Nested cgroup hierarchy with CFS bandwidth limits**: At least 3 levels of cgroups are needed (e.g., level2 → level3b → worker3). The bandwidth throttling must be enabled via `cpu.cfs_quota_us` and `cpu.cfs_period_us` on the parent groups.

2. **Specific throttle/unthrottle ordering**: The sequence must be:
   - A leaf task (worker3) in a deeply-nested cgroup gets throttled due to its ancestor's bandwidth limit being exhausted.
   - The intermediate cgroup's `cfs_rq` (level3b) fully decays its PELT load to zero (this happens naturally as time passes with no running entities).
   - A higher-level ancestor (level2) also gets throttled.
   - The leaf task's bandwidth is replenished first (worker3 unthrottled).
   - The higher-level ancestor's bandwidth is replenished (level2 unthrottled).

3. **Load decay timing**: The intermediate cfs_rq (level3b) must have fully decayed all PELT metrics (`load_sum == 0`, `util_sum == 0`, `runnable_sum == 0`, `load.weight == 0`) before the unthrottle walk occurs. This typically requires several hundred milliseconds of inactivity.

4. **`nr_running == 0` on intermediate node**: The intermediate cfs_rq must have no directly-enqueued entities. This is natural for intermediate nodes in deep hierarchies—they only have child task groups, not directly-running tasks.

5. **CONFIG_FAIR_GROUP_SCHED and CONFIG_SMP**: Both must be enabled (standard for most distributions).

6. **CONFIG_CFS_BANDWIDTH**: Must be enabled to use bandwidth throttling.

The bug is relatively easy to trigger with the LTP `cfs_bandwidth01` test, which exercises exactly this type of deep hierarchy with bandwidth limits. The reproduction is fairly reliable given the specific cgroup structure and enough load to trigger throttle/unthrottle cycles.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**

   The kernel version is too old. This bug was introduced by commit `a7b359fc6a37` which was merged into v5.13-rc7, and the fix `fdaba61ef8a268d4136d0a113d153f7a89eb9984` was also merged into v5.13 (the final release). kSTEP supports Linux v5.15 and newer only. At v5.15, this fix was already present in the kernel, so the buggy code path does not exist in any kernel version that kSTEP can target.

   Specifically, checking out the parent commit (`fdaba61ef8a268d4136d0a113d153f7a89eb9984~1`) would give a kernel at the v5.13-rc7 development stage, which is well below kSTEP's v5.15 minimum version requirement. The `#if LINUX_VERSION_CODE` guard in the driver would need to target a version range within v5.13-rc7 to v5.13, which kSTEP does not support.

2. **WHAT would need to be added to kSTEP to support this?**

   If version support were not an issue, kSTEP already has the necessary capabilities to reproduce this bug:
   - `kstep_cgroup_create()` can create nested cgroup hierarchies.
   - CFS bandwidth parameters can be set via `kstep_sysctl_write()` or direct cgroup interface writes.
   - `kstep_task_create()` and `kstep_cgroup_add_task()` can place tasks in specific cgroups.
   - `kstep_tick_repeat()` can advance time to trigger PELT decay.
   - Internal access to `cfs_rq->on_list`, `rq->tmp_alone_branch`, and `rq->leaf_cfs_rq_list` via `KSYM_IMPORT()` and `internal.h` would allow verification.
   
   The reproduction strategy would be:
   1. Create a 3-level cgroup hierarchy: `/ltp/test/level2/level3b/worker3`.
   2. Set bandwidth limits on level2 (e.g., `cpu.cfs_quota_us = 10000`, `cpu.cfs_period_us = 100000`).
   3. Create a busy task (worker3) pinned to CPU 1 inside the worker3 cgroup.
   4. Let it run until level2's bandwidth is exhausted and throttling occurs.
   5. Tick repeatedly to let level3b's PELT values decay to zero.
   6. Wait for bandwidth replenishment and unthrottling.
   7. Check if `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list` (should be true on fixed kernel, false on buggy kernel).

   However, none of this matters since the buggy kernel version (v5.13-rc7 to v5.13) is outside kSTEP's supported range.

3. **Version constraint**: The fix was merged into v5.13, which predates kSTEP's minimum supported version of v5.15. By the time v5.15 was released, this fix had been in the kernel for over 4 months.

4. **Alternative reproduction methods**: The bug can be reproduced outside kSTEP using the LTP (Linux Test Project) `cfs_bandwidth01` test on a v5.13-rc7 kernel. The test creates a nested cgroup hierarchy with bandwidth limits and runs stress workloads that trigger the specific throttle/unthrottle sequence needed. The `SCHED_WARN_ON` in the kernel log confirms the bug is triggered.
