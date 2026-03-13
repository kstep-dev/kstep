# PELT: Stale tg_load_avg_contrib After Blocked Load Propagation

**Commit:** `02da26ad5ed6ea8680e5d01f20661439611ed776`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.13-rc6
**Buggy since:** v5.1-rc1 (introduced by commit `039ae8bcf7a5` "sched/fair: Fix O(nr_cgroups) in the load balancing path")

## Bug Description

When the kernel updates blocked (decaying) PELT load averages in `__update_blocked_fair()`, it iterates over all leaf `cfs_rq` entries in bottom-up order. For each leaf `cfs_rq`, it first updates that `cfs_rq`'s own load average via `update_cfs_rq_load_avg()`, then propagates any pending load changes to the parent `cfs_rq` by calling `update_load_avg()` on the parent's scheduling entity. However, the propagation call passes `0` as the flags argument, meaning the `UPDATE_TG` flag is not set. This means even though the parent `cfs_rq`'s load average is modified (potentially becoming zero due to decay propagation), the parent's `tg_load_avg_contrib` — its contribution to the task group's global `tg->load_avg` — is not updated.

Later in the same iteration, when the loop reaches the parent `cfs_rq` itself, it calls `update_cfs_rq_load_avg()` again. But because the parent was already updated moments ago during the child's propagation, the PELT computation finds nothing to decay (the timestamps are current) and returns false. Consequently, `update_tg_load_avg()` is not called for the parent, and its `tg_load_avg_contrib` remains stale.

This creates a permanent mismatch: the parent `cfs_rq` has already decayed to zero (and is subsequently removed from the leaf list by `list_del_leaf_cfs_rq()`), but its old non-zero `tg_load_avg_contrib` value was never subtracted from `tg->load_avg`. The task group's global load average permanently retains a phantom contribution from a `cfs_rq` that no longer has any load.

This bug was reported by Odin Ugedal, who observed severe fairness problems between cgroups in production systems. Cgroups that should have received equal CPU shares were getting wildly different amounts because the inflated `tg->load_avg` caused incorrect weight calculations in `calc_group_shares()` (now `calc_group_runnable()`), which determines how much CPU time each cgroup receives relative to others.

## Root Cause

The root cause is a missing `UPDATE_TG` flag in the `update_load_avg()` call within `__update_blocked_fair()`. The buggy code at line ~8032 of fair.c (pre-fix) reads:

```c
/* Propagate pending load changes to the parent, if any: */
se = cfs_rq->tg->se[cpu];
if (se && !skip_blocked_update(se))
    update_load_avg(cfs_rq_of(se), se, 0);  /* BUG: flags=0, no UPDATE_TG */
```

Inside `update_load_avg()`, the function calls `update_cfs_rq_load_avg()` on the parent `cfs_rq` and `propagate_entity_load_avg()` on the scheduling entity. If either of these returns true (indicating the load has decayed), the function checks whether `flags & UPDATE_TG` is set before calling `update_tg_load_avg()`. Since flags is `0`, `update_tg_load_avg()` is never called, even though the parent `cfs_rq`'s load values have changed significantly.

The specific problematic sequence is:

1. **Iteration processes child `cfs_rq`**: `update_cfs_rq_load_avg()` decays the child's PELT values. This causes `add_tg_cfs_propagate()` to set `prop_runnable_sum` on the child `cfs_rq`, flagging pending propagation.

2. **Propagation to parent**: `update_load_avg(cfs_rq_of(se), se, 0)` is called. Inside, `update_cfs_rq_load_avg()` updates the parent `cfs_rq`'s timestamp and PELT values. `propagate_entity_load_avg()` detects the pending `prop_runnable_sum` and calls `update_tg_cfs_load()`, `update_tg_cfs_runnable()`, and `update_tg_cfs_util()`, which modify the parent `cfs_rq`'s `avg.load_avg`, potentially making it zero. However, since `UPDATE_TG` is not in flags, `update_tg_load_avg()` is skipped. The parent's `tg_load_avg_contrib` remains at its old (non-zero) value.

3. **Iteration reaches parent `cfs_rq`**: `update_cfs_rq_load_avg()` is called again for the parent, but since `last_update_time` was already set to the current time in step 2, no decay occurs, and the function returns 0 (no change). Therefore `update_tg_load_avg()` is still not called.

4. **Parent removed from leaf list**: `cfs_rq_is_decayed()` returns true because all load/util/runnable sums are now zero. `list_del_leaf_cfs_rq()` removes the parent from the list. But `tg_load_avg_contrib` was never zeroed, so `tg->load_avg` still includes the stale contribution.

The function `update_tg_load_avg()` computes `delta = cfs_rq->avg.load_avg - cfs_rq->tg_load_avg_contrib` and adds it to `tg->load_avg` atomically. If `cfs_rq->avg.load_avg` has gone to 0 but `tg_load_avg_contrib` is still, say, 1024, then delta would be -1024 and `tg->load_avg` would be correctly decremented. But since `update_tg_load_avg()` was never called, this correction never happens.

## Consequence

The primary consequence is **unfair CPU distribution between cgroups**. The `tg->load_avg` value is used by `calc_group_shares()` (or `calc_group_runnable()`) to determine the effective weight of a task group's scheduling entity. The formula is essentially:

```
shares = tg->shares * cfs_rq->avg.load_avg / tg->load_avg
```

When `tg->load_avg` is artificially inflated by stale `tg_load_avg_contrib` values from `cfs_rq`s that have already decayed, the computed shares for active `cfs_rq`s are reduced. This means cgroups get less CPU time than they should. In Odin Ugedal's reproduction, two cgroups with equal weight that should receive 50/50 CPU time could instead get wildly skewed ratios because one cgroup's task group had a permanently inflated `load_avg`.

The bug is particularly insidious because once a `cfs_rq` is removed from the leaf list, it will never be iterated again, so the stale contribution is permanent until the cgroup is destroyed. The effect accumulates over time: every time a task migrates between CPUs and the old CPU's `cfs_rq` decays under the right conditions, another stale contribution gets locked in. In production Kubernetes clusters with many cgroups and frequent task migrations, this can lead to significant and persistent fairness violations.

Additionally, the stale `tg->load_avg` value can cause issues with load balancing decisions. Functions like `update_cfs_rq_h_load()` use `tg->load_avg` to compute hierarchical loads for migration decisions. An inflated `tg->load_avg` makes the load balancer think a cgroup has more load spread across CPUs than it actually does, potentially causing incorrect migration decisions.

## Fix Summary

The fix is a one-line change in `__update_blocked_fair()` that adds the `UPDATE_TG` flag to the `update_load_avg()` call when propagating load changes to the parent:

```c
/* Before (buggy): */
update_load_avg(cfs_rq_of(se), se, 0);

/* After (fixed): */
update_load_avg(cfs_rq_of(se), se, UPDATE_TG);
```

With `UPDATE_TG` set, when `update_load_avg()` detects that the parent `cfs_rq`'s load has changed (either through `update_cfs_rq_load_avg()` returning non-zero or `propagate_entity_load_avg()` returning non-zero), it will call `update_tg_load_avg(cfs_rq)` on the parent. This ensures that `tg_load_avg_contrib` is synchronized with `cfs_rq->avg.load_avg` before the `cfs_rq` is potentially removed from the leaf list.

This fix is correct and complete because the only path where `tg_load_avg_contrib` could become stale was this propagation path in `__update_blocked_fair()`. All other callers of `update_load_avg()` that can modify a `cfs_rq`'s load already pass `UPDATE_TG` (e.g., `enqueue_entity()`, `dequeue_entity()`, `put_prev_entity()`). The blocked load update path was the sole exception, introduced when commit `039ae8bcf7a5` added the optimization to remove decayed `cfs_rq`s from the leaf list. The removal optimization was correct in principle but needed the `UPDATE_TG` flag to ensure the accounting was updated before removal.

This commit is the second of a two-patch series. The first patch (`sched/fair: keep load_avg and load_sum synced`) fixed a related issue where `load_sum` could become zero while `load_avg` was non-zero due to integer truncation during propagation. Together, these two patches ensure that `tg_load_avg_contrib` always reflects the actual state of the `cfs_rq`'s load average, and that the `cfs_rq` is not prematurely removed from the leaf list.

## Triggering Conditions

The bug requires the following conditions:

1. **CONFIG_FAIR_GROUP_SCHED enabled (cgroup v1 or v2 with cpu controller)**: The bug only manifests with task groups, since `tg_load_avg_contrib` and `tg->load_avg` are only relevant for task group scheduling.

2. **At least a 2-level cgroup hierarchy**: There must be a child `cfs_rq` and a parent `cfs_rq` in the leaf list. The parent must have its scheduling entity (`tg->se[cpu]`) on another `cfs_rq`. The root `cfs_rq` (`rq->cfs`) is excluded from the `update_tg_load_avg()` check.

3. **Tasks that migrate or block, leaving cfs_rqs to decay**: The child `cfs_rq` must have had recent activity (enqueue/dequeue) that set `prop_runnable_sum` for propagation. Then all tasks must leave (migrate away or block), so the load starts decaying toward zero.

4. **Specific iteration ordering**: The `for_each_leaf_cfs_rq_safe` loop processes child `cfs_rq`s before parent `cfs_rq`s (bottom-up). The child must be processed first, triggering propagation that updates the parent's PELT values. Then when the parent is processed, its PELT has already been updated (no further decay), so `update_tg_load_avg()` is skipped.

5. **Multiple CPUs**: The bug is most impactful with multiple CPUs because tasks can migrate between them. When a task migrates from CPU A to CPU B, CPU A's `cfs_rq` for that task group starts decaying. If the decay happens under the right propagation conditions, the stale `tg_load_avg_contrib` gets locked in.

The reproduction scenario from Odin Ugedal's report involves:
- Two cgroups (`cg-1` and `cg-2`) each with a CPU-bound stress task
- `cg-1`'s task is moved between CPUs (via cpuset changes: CPU 0 → 1 → 2 → 3)
- Each migration leaves behind a decaying `cfs_rq` on the old CPU
- CFS bandwidth throttling (`cpu.max = "10000 100000"`) on the parent cgroup accelerates the creation of blocked/decaying `cfs_rq`s
- After settling, `tg_load_avg_contrib` and `tg->load_avg` diverge visibly

The bug is relatively easy to trigger in practice: any workload with cgroups and task migration will eventually hit it. The use of bandwidth throttling makes it more reproducible because throttling causes all tasks in a `cfs_rq` to be dequeued simultaneously, leading to a clean decay scenario.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP using the following approach:

### Setup

1. **CPU count**: Configure QEMU with at least 3 CPUs (e.g., CPUs 1, 2, 3; CPU 0 is reserved for the driver).

2. **Cgroup hierarchy**: Create a 2-level cgroup hierarchy:
   - `/cg_parent` — the parent cgroup
   - `/cg_parent/cg_child` — a child cgroup
   
   Use `kstep_cgroup_create("cg_parent")` and `kstep_cgroup_create("cg_parent/cg_child")`. Set weights with `kstep_cgroup_set_weight()`.

3. **Tasks**: Create CFS tasks using `kstep_task_create()`. Place them in the child cgroup using `kstep_cgroup_add_task("cg_parent/cg_child", pid)`. Pin them to a specific CPU (e.g., CPU 1) using `kstep_task_pin(p, 1, 2)`.

### Triggering Sequence

4. **Build up load**: Wake the tasks with `kstep_task_wakeup()` and run ticks with `kstep_tick_repeat()` for enough ticks (e.g., 100+) to build up PELT load averages on both the child `cfs_rq` and the parent `cfs_rq` on CPU 1.

5. **Block all tasks**: Call `kstep_task_block()` on all tasks in the child cgroup. This begins the decay process for both child and parent `cfs_rq`s on CPU 1.

6. **Trigger blocked load update**: Run ticks with `kstep_tick_repeat()`. The scheduler periodically calls `update_blocked_averages()` which invokes `__update_blocked_fair()`. During these updates, the child `cfs_rq`'s load decays. When propagating the child's decay to the parent, the parent `cfs_rq`'s load is updated without `UPDATE_TG`, so `tg_load_avg_contrib` is not updated.

7. **Continue ticking until fully decayed**: Keep ticking until the parent `cfs_rq` is fully decayed (all PELT sums zero) and removed from the leaf list. This may take many ticks (PELT has a half-life of ~32ms, so at 4ms tick interval, ~50-100 ticks to decay sufficiently).

### Detection

8. **Observe tg_load_avg_contrib**: Use `KSYM_IMPORT()` to access internal scheduler symbols. After the `cfs_rq` has been removed from the leaf list, read `tg->load_avg` using `atomic_long_read(&tg->load_avg)`. On the buggy kernel, `tg->load_avg` will be non-zero even though no `cfs_rq` has any load. On the fixed kernel, `tg->load_avg` will be zero (or very close to zero).

9. **Alternatively**, use `on_sched_softirq_end` callback to monitor `cfs_rq->tg_load_avg_contrib` and `cfs_rq->avg.load_avg` during each blocked update iteration. On the buggy kernel, after the parent `cfs_rq` is processed, `tg_load_avg_contrib` will remain at a stale non-zero value even as `avg.load_avg` drops to zero. On the fixed kernel, they will stay synchronized.

### Pass/Fail Criteria

10. **After all tasks have been blocked for a long time** (enough for full PELT decay), check:
    - On buggy kernel: `atomic_long_read(&tg->load_avg) > 0` when no task in the group is runnable anywhere → **FAIL** (stale contribution detected).
    - On fixed kernel: `atomic_long_read(&tg->load_avg) == 0` (or within a small epsilon due to rounding) → **PASS**.

### Implementation Notes

- Access `struct task_group *tg` via the `cfs_rq->tg` pointer obtained from `cpu_rq(cpu)->cfs` or from the cgroup's internal structures. Use `KSYM_IMPORT()` if needed to get `css_tg()` or similar helpers.
- The `tg_load_avg_contrib` field is on each per-CPU `cfs_rq` within the task group. Access it via `tg->cfs_rq[cpu]->tg_load_avg_contrib`.
- The `on_sched_softirq_end` callback is ideal for checking state after `update_blocked_averages()` has run.
- The bug requires `CONFIG_SMP` and `CONFIG_FAIR_GROUP_SCHED` to be enabled, which are default in most kernel configurations.
- Make sure the child cgroup hierarchy is deep enough (at least one intermediate task group between root and the leaf) so that the propagation path in `__update_blocked_fair()` is exercised.
- Guard the driver with `#if LINUX_VERSION_CODE` for kernel versions between v5.1 and v5.13.
