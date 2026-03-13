# Bandwidth: Stale runnable_avg on CFS bandwidth throttle/unthrottle

**Commit:** `6212437f0f6043e825e021e4afc5cd63e248a2b4`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.7-rc1
**Buggy since:** v5.7-rc1 (introduced by `9f68395333ad` "sched/pelt: Add a new runnable average signal", both commits merged in the same tip/sched/core batch)

## Bug Description

When CFS bandwidth throttling is enabled and a `cfs_rq` is throttled (because its task group has exhausted its CPU bandwidth quota), the kernel dequeues the group's scheduling entity (`se`) from its parent `cfs_rq` and decrements `h_nr_running` up the hierarchy. Similarly, when the `cfs_rq` is unthrottled (bandwidth is replenished), the entity is re-enqueued and `h_nr_running` is incremented back.

The bug is that during these throttle/unthrottle hierarchy walks, the `runnable_avg` PELT (Per-Entity Load Tracking) signal and the group entity's `runnable_weight` are not properly updated for ancestor scheduling entities that are **not** themselves being enqueued or dequeued. In a multi-level cgroup hierarchy (e.g., `/cg_a/cg_b/tasks`), only the lowest ancestor entity that has no remaining load gets dequeued/enqueued. Higher-level ancestor entities that remain on their run queues (because they still have other children with load) skip the `dequeue_entity()`/`enqueue_entity()` call and thus skip the `update_load_avg()` and `se_update_runnable()` calls that those functions perform internally.

This means that after throttling, higher-level group entities carry stale `runnable_weight` values (still reflecting the old `h_nr_running` that included the now-throttled tasks) and the parent `cfs_rq`'s `runnable_avg` is not updated to reflect the reduced runnable pressure. The inverse problem occurs on unthrottle: the `runnable_avg` is not updated to reflect the increased runnable pressure from the re-added tasks.

The `runnable_avg` signal was introduced by commit `9f68395333ad` to track per-entity and per-`cfs_rq` runnable time (running + waiting time) to improve load balancing decisions. The signal relies on `se->runnable_weight` (set to `se->my_q->h_nr_running` for group entities) being kept in sync with the actual number of runnable tasks in the group hierarchy. When throttle/unthrottle operations modify `h_nr_running` without updating the PELT machinery, this invariant is violated.

## Root Cause

The root cause lies in the `throttle_cfs_rq()` and `unthrottle_cfs_rq()` functions in `kernel/sched/fair.c`. Both functions walk up the scheduling entity hierarchy using `for_each_sched_entity(se)` and adjust `qcfs_rq->h_nr_running` at each level. They use a `dequeue`/`enqueue` flag to determine whether the entity at the current level should be fully dequeued/enqueued.

In `throttle_cfs_rq()`, the loop is:
```c
for_each_sched_entity(se) {
    struct cfs_rq *qcfs_rq = cfs_rq_of(se);
    if (!se->on_rq)
        break;

    if (dequeue)
        dequeue_entity(qcfs_rq, se, DEQUEUE_SLEEP);  // calls update_load_avg + se_update_runnable
    /* BUG: no else branch — skipped entities get no PELT update */

    qcfs_rq->h_nr_running -= task_delta;
    qcfs_rq->idle_h_nr_running -= idle_task_delta;

    if (qcfs_rq->load.weight)
        dequeue = 0;  // stop dequeuing at levels with remaining load
}
```

The critical issue is that `dequeue_entity()` internally calls `update_load_avg(cfs_rq, se, UPDATE_TG)` and `se_update_runnable(se)`. The `se_update_runnable()` function sets `se->runnable_weight = se->my_q->h_nr_running` for group entities. The `update_load_avg()` function recalculates PELT averages and propagates load changes up the hierarchy.

When `dequeue` becomes 0 (because `qcfs_rq->load.weight` is nonzero, meaning the parent `cfs_rq` still has other entities with weight), the code simply decrements `h_nr_running` without calling `update_load_avg()` or `se_update_runnable()`. This means:

1. The entity's `runnable_weight` still reflects the **old** `h_nr_running` (before the throttled tasks were subtracted), because `se_update_runnable()` reads `se->my_q->h_nr_running` which was already decremented at the child level but the entity at this level hasn't been updated yet.
2. The parent `cfs_rq`'s PELT `runnable_avg` is not updated to account for the change in runnable weight, because `update_load_avg()` was not called.

The same problem exists symmetrically in `unthrottle_cfs_rq()` where `enqueue` becomes 0 for higher-level entities that are already on their run queue.

The function `se_update_runnable()` is defined in `kernel/sched/sched.h` as:
```c
static inline void se_update_runnable(struct sched_entity *se)
{
    if (!entity_is_task(se))
        se->runnable_weight = se->my_q->h_nr_running;
}
```

This function must be called **after** `h_nr_running` has been modified at the child level, so the parent entity's `runnable_weight` reflects the new count. The ordering in the loop is crucial: `h_nr_running` is decremented/incremented after the dequeue/enqueue call, but for the skipped entities (no dequeue/enqueue), `se_update_runnable()` was not called at all.

## Consequence

The consequence is incorrect PELT `runnable_avg` values for `cfs_rq`s in multi-level cgroup hierarchies when CFS bandwidth throttling is active. This manifests as:

1. **Incorrect load balancing decisions**: The `runnable_avg` signal is used by the load balancer (via `cfs_rq_runnable_avg()`) to assess runnable pressure on each CPU's run queue. Stale `runnable_avg` values can cause the load balancer to misclassify CPUs as overloaded (when tasks have been throttled but `runnable_avg` still reflects them as runnable) or underloaded (when tasks have been unthrottled but `runnable_avg` hasn't caught up). This can lead to suboptimal task placement and unnecessary or missed task migrations.

2. **Stale group entity weights**: The `se->runnable_weight` is used by the scheduler to compute group entity load contributions. When it is stale, the effective weight of group entities in the CFS scheduling tree is incorrect, which can lead to unfair CPU time distribution among task groups. Tasks in unrelated cgroups may receive more or less CPU time than their configured share.

3. **Accumulated error over time**: Because PELT signals are exponentially weighted moving averages, a stale value that persists across multiple throttle/unthrottle cycles can compound, causing the `runnable_avg` to diverge increasingly from the true runnable pressure. The error only self-corrects slowly through the PELT decay mechanism, or when a full dequeue/enqueue cycle happens to propagate the correct values.

This bug does not cause crashes or kernel panics. It is a correctness issue affecting scheduling fairness and load balancing quality in cgroup-based bandwidth-limited workloads with nested cgroup hierarchies (at least 3 levels deep to trigger the `dequeue = 0`/`enqueue = 0` path).

## Fix Summary

The fix adds explicit `update_load_avg()` and `se_update_runnable()` calls in the `else` branches of both `throttle_cfs_rq()` and `unthrottle_cfs_rq()`. For entities that are not being fully dequeued/enqueued (because they still have other children contributing load), the fix ensures that:

1. `update_load_avg(qcfs_rq, se, 0)` is called to update the entity's PELT averages and the parent `cfs_rq`'s aggregate PELT signals. The `0` flags argument means no `UPDATE_TG` or `DO_ATTACH`, as this is a lightweight update that just needs to resync the PELT state with the current time and propagate any pending changes.

2. `se_update_runnable(se)` is called to set `se->runnable_weight = se->my_q->h_nr_running`, picking up the new `h_nr_running` value that was decremented/incremented in the previous iteration of the loop (at the child level).

In `throttle_cfs_rq()`, the fix changes:
```c
if (dequeue) {
    dequeue_entity(qcfs_rq, se, DEQUEUE_SLEEP);
} else {
    update_load_avg(qcfs_rq, se, 0);
    se_update_runnable(se);
}
```

And symmetrically in `unthrottle_cfs_rq()`:
```c
if (enqueue) {
    enqueue_entity(cfs_rq, se, ENQUEUE_WAKEUP);
} else {
    update_load_avg(cfs_rq, se, 0);
    se_update_runnable(se);
}
```

This fix is correct because it mirrors what `enqueue_entity()` and `dequeue_entity()` do internally: both call `update_load_avg()` followed by `se_update_runnable()` as their first operations. For entities that remain on the run queue, only these PELT-related updates are needed; there is no need to perform the full enqueue/dequeue (tree insertion/removal, accounting, vruntime adjustment, etc.).

## Triggering Conditions

- **Kernel configuration**: `CONFIG_FAIR_GROUP_SCHED=y`, `CONFIG_CFS_BANDWIDTH=y`, `CONFIG_SMP=y` (for PELT load tracking to be meaningful).
- **Cgroup hierarchy**: At least 3 levels deep (root → group A → group B → tasks) so that when group B's `cfs_rq` is throttled, the walk up the hierarchy has at least one ancestor entity (group A's `se`) that remains on the run queue (i.e., `dequeue` becomes 0 because group A's parent `cfs_rq` still has weight from other children).
- **Bandwidth quota**: Group B must have a CFS bandwidth quota configured (e.g., `cpu.cfs_quota_us` and `cpu.cfs_period_us` set) that is small enough to be exhausted, triggering throttling.
- **Multiple task groups**: To trigger the `dequeue = 0` path, the parent cfs_rq at the group A level must have `load.weight > 0`, meaning there must be other entities (tasks or groups) with weight in that cfs_rq. This ensures the parent entity is not dequeued.
- **Observation**: The bug manifests as stale `runnable_avg` values and stale `se->runnable_weight` values at intermediate hierarchy levels. It requires reading these PELT fields to detect, or observing incorrect load balancing behavior as a downstream effect.
- **Timing**: The bug triggers deterministically whenever a throttle or unthrottle event occurs in a qualifying hierarchy. It is not a race condition.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The kernel version is too old. The fix commit `6212437f0f6043e825e021e4afc5cd63e248a2b4` was merged into `v5.7-rc1`, and the bug was introduced by `9f68395333ad` in the same `v5.7-rc1` merge window. kSTEP supports Linux v5.15 and newer only. Since the buggy code exists only in kernels from the v5.7 development cycle (and the fix was also included in v5.7-rc1), this predates kSTEP's supported kernel range by several major versions.

2. **WHAT would need to be added to kSTEP?**
   If kSTEP supported v5.7-era kernels, the bug itself would be conceptually reproducible. A kSTEP driver would need to:
   - Create a nested cgroup hierarchy (3+ levels) with CFS bandwidth quotas
   - Spawn tasks in the innermost cgroup
   - Allow the tasks to consume their bandwidth quota and trigger throttling
   - Read the `runnable_avg` and `runnable_weight` fields of intermediate group scheduling entities before and after throttle events
   - Compare observed values against expected values

   The existing kSTEP cgroup APIs (`kstep_cgroup_create`, `kstep_cgroup_set_weight`) and task APIs would suffice for the workload setup. Reading PELT fields via `KSYM_IMPORT` and `cfs_rq`/`se` internals is already supported. The only barrier is kernel version compatibility.

3. **Version constraint**: The fix targets v5.7-rc1, which is pre-v5.15. kSTEP supports v5.15+ only.

4. **Alternative reproduction methods**:
   - Build a v5.7-rc1 kernel (without the fix, i.e., with commit `9f68395333ad` applied but before `6212437f0f6043e825e021e4afc5cd63e248a2b4`) and set up a nested cgroup hierarchy with bandwidth limits
   - Use `perf sched` or ftrace to observe PELT signal values at throttle/unthrottle events
   - Write a BPF program or use tracepoints (`sched:pelt_cfs_tp`, `sched:pelt_se_tp`) to capture `runnable_avg` values before and after throttle events
   - Compare `se->runnable_weight` against the actual `h_nr_running` of the child `cfs_rq` at intermediate hierarchy levels to detect staleness
