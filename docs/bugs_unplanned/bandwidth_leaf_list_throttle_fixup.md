# Bandwidth: Leaf CFS RQ List Corruption During Enqueue With Throttled Hierarchy

**Commit:** `b34cb07dde7c2346dec73d053ce926aeaa087303`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.7-rc7
**Buggy since:** v5.7-rc1 (introduced by `fe61468b2cbc` "sched/fair: Fix enqueue_task_fair warning")

## Bug Description

The `enqueue_task_fair()` function in the CFS scheduler maintains a per-runqueue linked list called `leaf_cfs_rq_list` that tracks all CFS runqueues in proper hierarchical order. This list is critical for correctly iterating over CFS runqueues during various scheduler operations (e.g., update_blocked_averages). A temporary pointer `rq->tmp_alone_branch` is used during enqueue operations to track an in-progress branch insertion, and an assertion (`assert_list_leaf_cfs_rq`) verifies at the end of `enqueue_task_fair()` that `tmp_alone_branch` has been properly reset to `&rq->leaf_cfs_rq_list`.

An earlier fix (`fe61468b2cbc`) had addressed a related issue where throttled CFS runqueues could be removed from the leaf list while having `nr_running > 1`, and a subsequent task enqueue into the throttled branch would fail to re-add them properly. That fix unconditionally called `list_add_leaf_cfs_rq()` in `enqueue_entity()` when CFS bandwidth control was enabled, and added a third `for_each_sched_entity` cleanup loop at the end of `enqueue_task_fair()`.

However, the earlier fix was incomplete. There is a specific scenario involving the interaction between the three `for_each_sched_entity` loops in `enqueue_task_fair()` where the leaf list can still end up in an inconsistent state. This occurs when the first loop exits early because a parent sched_entity is already `on_rq`, a child's `list_add_leaf_cfs_rq()` call encounters a parent not yet on the list (setting `tmp_alone_branch` to point into an incomplete branch), and the second loop then advances `se` further up the hierarchy before breaking out due to throttling. The third cleanup loop, which iterates using `se`, then starts from the wrong sched_entity position and fails to repair the leaf list, leaving `tmp_alone_branch` dangling.

## Root Cause

The root cause lies in the interaction between the three `for_each_sched_entity` loops in `enqueue_task_fair()` and how the `se` variable is shared and modified across them.

**The `enqueue_task_fair()` function has three loops:**

1. **First loop (enqueue loop):** Walks up from the task's sched_entity, calling `enqueue_entity()` for each level. It breaks out early if `se->on_rq` is already true (meaning this ancestor is already enqueued). Inside `enqueue_entity()`, `list_add_leaf_cfs_rq(cfs_rq)` is called to add the CFS runqueue to the leaf list. When a cfs_rq's parent is not yet on the list, `list_add_leaf_cfs_rq()` enters the third case: it adds the cfs_rq at `rq->tmp_alone_branch` and updates `tmp_alone_branch` to point to `&cfs_rq->leaf_cfs_rq_list`, returning `false`. This signals that the branch is incomplete — the parent still needs to be added later to connect the branch and reset `tmp_alone_branch`.

2. **Second loop (update loop):** Continues from where `se` left off (which is the sched_entity that was already `on_rq`), walking further up the hierarchy to update load averages. This loop can also break early via `goto enqueue_throttle` if it encounters a throttled CFS runqueue. Critically, this loop modifies `se` as it iterates, advancing it past the point where the first loop stopped.

3. **Third loop (cleanup loop):** Runs only when `cfs_bandwidth_used()` is true. It iterates using `se` (which was modified by the second loop) and calls `list_add_leaf_cfs_rq(cfs_rq)` for each level, breaking when the function returns `true` (meaning the branch is fully connected). However, because `se` was advanced by the second loop, this cleanup loop may start at a sched_entity that is *above* the level where the incomplete branch was created. It therefore misses the cfs_rqs that actually need to be added to fix the list.

**The specific failure scenario (from Phil Auld's trace):**

Consider a cgroup hierarchy with at least 3 levels (root → A → B → task). Task is being enqueued:

1. First loop iteration 1: `se` = task's sched_entity. `enqueue_entity()` is called. Inside, `list_add_leaf_cfs_rq(cfs_rq_B)` is called. cfs_rq_B's parent (cfs_rq_A) is not on the list, so it enters the third case: adds cfs_rq_B at `tmp_alone_branch` and sets `tmp_alone_branch = &cfs_rq_B->leaf_cfs_rq_list`. Returns `false`.

2. First loop iteration 2: `se` = group_se_A (sched_entity for group A). `se->on_rq` is 1 (because another task in a parallel child hierarchy had already enqueued it). Loop breaks. `se` points to group_se_A. At this point, `tmp_alone_branch` still points into cfs_rq_B's leaf_cfs_rq_list — the branch is incomplete.

3. Second loop starts from `se` = group_se_A. It processes cfs_rq_of(group_se_A) = some higher-level cfs_rq. On the next iteration, `se` advances to the parent. The loop then encounters a throttled cfs_rq and jumps to `enqueue_throttle`. Now `se` points to a sched_entity *above* group_se_A.

4. At `enqueue_throttle`, `se` is not NULL (it was set by the second loop), so `add_nr_running` is skipped.

5. Third cleanup loop iterates from the *current* `se`, which is above where the incomplete branch was. For each cfs_rq at these higher levels, `list_add_leaf_cfs_rq()` finds them already `on_list` (they were added by an earlier parallel enqueue). It returns `true` immediately. The loop never reaches cfs_rq_A, which is the one that needs to be added to connect cfs_rq_B's branch. `tmp_alone_branch` remains pointing to `&cfs_rq_B->leaf_cfs_rq_list`.

6. `assert_list_leaf_cfs_rq(rq)` fires: `SCHED_WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list)`.

The key insight is that between the first and second loop, a throttled parent in the hierarchy causes the second loop to both advance `se` beyond the problematic level and to break early. The cleanup code then operates on the wrong part of the hierarchy.

## Consequence

The immediate observable consequence is a `SCHED_WARN_ON` assertion failure in `assert_list_leaf_cfs_rq()`:

```
SCHED_WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list)
```

This produces a kernel warning (a stack trace in dmesg) on every affected enqueue operation. Phil Auld's trace confirmed this was "100% reproducible" and occurred repeatedly on a production system.

Beyond the warning, the corrupted `leaf_cfs_rq_list` can cause incorrect scheduler behavior. The list is used by `for_each_leaf_cfs_rq_safe()` to iterate over CFS runqueues for operations like `update_blocked_averages()`, `__update_blocked_fair()`, and hierarchical load accounting. If a cfs_rq is disconnected from the leaf list (not properly linked), its PELT (Per-Entity Load Tracking) metrics may stop being updated. This can lead to stale load/utilization values, affecting load balancing decisions, CPU frequency scaling (schedutil), and EAS (Energy Aware Scheduling) decisions.

In the worst case, a permanently corrupted leaf list could lead to tasks being "invisible" to the load balancer in throttled hierarchies, causing load imbalances, scheduling latency anomalies, or even soft lockups if list iteration encounters corrupted pointers. The `SCHED_WARN_ON` itself, while non-fatal, generates significant noise in kernel logs and may cause automated monitoring systems to flag the kernel as unhealthy.

## Fix Summary

The fix by Phil Auld (suggested by Vincent Guittot) takes a different approach from the original patch proposal. Instead of saving and restoring the `se` pointer (which was Phil's initial v1 patch), the accepted fix adds a `list_add_leaf_cfs_rq(cfs_rq)` call directly inside the second `for_each_sched_entity` loop, guarded by `throttled_hierarchy(cfs_rq)`.

Specifically, in the second loop (the update loop), after processing each sched_entity's cfs_rq (updating load averages, checking for throttling), the fix adds:

```c
if (throttled_hierarchy(cfs_rq))
    list_add_leaf_cfs_rq(cfs_rq);
```

This ensures that whenever the second loop encounters a cfs_rq that is part of a throttled hierarchy (i.e., it has a throttled ancestor), the cfs_rq is added to the leaf list immediately. This proactively repairs incomplete branches as the second loop traverses up the hierarchy. By the time the third cleanup loop runs (or doesn't run), the leaf list is already consistent because all intermediate cfs_rqs have been properly linked.

This fix is more robust than the save/restore approach because it handles all cases where the second loop modifies `se` and encounters throttled hierarchies, not just the specific case where the first loop broke out due to `on_rq`. It is also simpler and more localized — a single conditional check inside the existing loop, rather than adding new state variables and restoration logic.

## Triggering Conditions

The bug requires all of the following conditions to be met simultaneously:

- **CFS bandwidth control must be enabled** (`CONFIG_CFS_BANDWIDTH=y` and active cfs_bandwidth configuration on at least one cgroup). This is needed for `cfs_bandwidth_used()` to return true and for cfs_rqs to be throttled.

- **A cgroup hierarchy with at least 3 levels** (root → group A → group B → task). The task being enqueued must be at a depth where its cfs_rq's parent can be in a state where it is not on the leaf list, while the grandparent is already on the list and on_rq.

- **A parent cfs_rq that is already `on_rq`** (i.e., `se->on_rq == 1` for a group sched_entity in the hierarchy). This causes the first loop to break early, leaving the leaf list incomplete (with `tmp_alone_branch` not reset to the list head).

- **A throttled ancestor above the `on_rq` parent.** This causes the second loop to break early (via `goto enqueue_throttle`) after advancing `se` past the level where the leaf list needs repair.

- **Parallel enqueue activity** in a sibling cgroup hierarchy. The parent sched_entity is `on_rq` because another task in a parallel child hierarchy was enqueued, which also added the parent's cfs_rq to the leaf list. However, the child cfs_rq's parent was not on the list at the time the child was added, creating the incomplete branch.

- **At least 2 CPUs** are needed (though the bug is per-CPU, the parallel enqueue scenario typically involves multiple tasks on the same CPU within different sub-hierarchies of the same cgroup tree).

The bug is deterministic once the conditions are met — Phil Auld confirmed it was "100% reproducible" in his test environment.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP.

**1. WHY can this bug not be reproduced with kSTEP?**

The primary reason is that the fix targets kernel v5.7-rc7, and the bug was introduced in v5.7-rc1 by commit `fe61468b2cbc`. kSTEP supports Linux v5.15 and newer only. The v5.7 kernel is significantly older than the minimum supported version, and the leaf_cfs_rq_list management code has undergone substantial refactoring between v5.7 and v5.15 (including commit `39f23ce07b93` "sched/fair: Fix unthrottle_cfs_rq() for leaf_cfs_rq list" and `51bf903b64bd` "sched/fair: Optimize and simplify rq leaf_cfs_rq_list"). The `tmp_alone_branch` mechanism and the three-loop structure of `enqueue_task_fair()` may have changed in later versions.

**2. WHAT would need to be added to kSTEP to support this?**

Even if the version constraint were removed, reproducing this bug would require:
- kSTEP support for CFS bandwidth control configuration (setting quota/period on cgroups). kSTEP already has `kstep_cgroup_create()` and `kstep_cgroup_set_weight()`, but would need `kstep_cgroup_set_bandwidth(name, quota, period)` or similar to configure CFS bandwidth throttling.
- The ability to create nested cgroup hierarchies (at least 3 levels deep) and place tasks in them. kSTEP has `kstep_cgroup_create()` but it's unclear if nested hierarchies are supported.
- Precise control over the timing of task enqueue operations to ensure the right ordering of events (task A enqueued making parent on_rq, then a parent getting throttled, then task B enqueued hitting the bug path).

**3. Kernel version constraint:**

The fix is in v5.7-rc7, which is before v5.15. kSTEP supports v5.15+ only. This is the primary disqualifying factor.

**4. Alternative reproduction methods outside kSTEP:**

The bug could be reproduced on a v5.7-rc1 through v5.7-rc6 kernel by:
- Configuring systemd or manually creating nested cgroups (3+ levels deep) with CFS bandwidth limits (cpu.cfs_quota_us / cpu.cfs_period_us).
- Running multiple CPU-intensive tasks in sibling sub-hierarchies of the cgroup tree, with tight bandwidth limits that cause frequent throttling.
- Monitoring dmesg for the `SCHED_WARN_ON` assertion failure.
- Phil Auld's trace shows this was reproducible on a real system with standard cgroup configurations. Adding `trace_printk` instrumentation to the scheduler helped diagnose the exact sequence of events.
