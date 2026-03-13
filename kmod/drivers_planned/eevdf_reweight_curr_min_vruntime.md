# EEVDF: Missing min_vruntime Update When Reweighting Current Entity

**Commit:** `5068d84054b766efe7c6202fc71b2350d1c326f1`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.8-rc1
**Buggy since:** v6.7-rc2 (introduced by `eab03c23c2a1` "sched/eevdf: Fix vruntime adjustment on reweight")

## Bug Description

When the EEVDF scheduler reweights a scheduling entity that is the currently running task (`cfs_rq->curr`), it fails to call `update_min_vruntime()` afterward. The `reweight_entity()` function adjusts the entity's vruntime via `reweight_eevdf()` to preserve lag invariants, but the code only calls `update_min_vruntime()` for non-current entities (those in the RB-tree). Since the current entity's vruntime participates in the `update_min_vruntime()` calculation (via the `if (curr)` branch inside `update_min_vruntime()`), skipping this update when the current entity is reweighted leaves `cfs_rq->min_vruntime` stale.

This bug was introduced by commit `eab03c23c2a1` ("sched/eevdf: Fix vruntime adjustment on reweight"), which restructured `reweight_entity()` to properly handle EEVDF vruntime adjustments. That commit introduced the `curr` local variable and split the tail logic into `if (!curr) { __enqueue_entity(); update_min_vruntime(); }`, correctly re-enqueueing non-current entities into the RB-tree and then updating min_vruntime. However, it inadvertently placed the `update_min_vruntime()` call inside the `if (!curr)` block, meaning that when the entity being reweighted *is* the current entity, `update_min_vruntime()` is never called despite the entity's vruntime having been modified by `reweight_eevdf()`.

The reweight path is commonly triggered through `update_cfs_group()`, which is called from `entity_tick()`, `enqueue_entity()`, `dequeue_entity()`, and `put_prev_entity()` on group scheduling entities. When a task group's proportional share changes (due to tasks being added/removed from sibling cfs_rqs), the group entity's weight is recalculated via `calc_group_shares()`, and if it differs from the current weight, `reweight_entity()` is called. Since `entity_tick()` explicitly calls `update_cfs_group(curr)`, the currently running group entity is a frequent target of reweighting.

## Root Cause

The root cause is a logic error in `reweight_entity()` where the `update_min_vruntime()` call is conditionally guarded by `!curr` when it should be called unconditionally for all on-rq entities.

In the buggy code at the end of `reweight_entity()`:

```c
if (se->on_rq) {
    update_load_add(&cfs_rq->load, se->load.weight);
    if (!curr) {
        __enqueue_entity(cfs_rq, se);
        update_min_vruntime(cfs_rq);
    }
}
```

The `__enqueue_entity()` call is correctly guarded by `!curr` because the current entity is not in the RB-tree (it was removed at pick time and will be re-inserted at `put_prev_entity()`). However, `update_min_vruntime()` was placed inside the same conditional block, making it execute only for non-current entities.

The `update_min_vruntime()` function considers both the leftmost entity in the RB-tree and the currently running entity:

```c
static void update_min_vruntime(struct cfs_rq *cfs_rq)
{
    struct sched_entity *se = __pick_root_entity(cfs_rq);
    struct sched_entity *curr = cfs_rq->curr;
    u64 vruntime = cfs_rq->min_vruntime;

    if (curr) {
        if (curr->on_rq)
            vruntime = curr->vruntime;
        else
            curr = NULL;
    }

    if (se) {
        if (!curr)
            vruntime = se->min_vruntime;
        else
            vruntime = min_vruntime(vruntime, se->min_vruntime);
    }

    u64_u32_store(cfs_rq->min_vruntime,
                  __update_min_vruntime(cfs_rq, vruntime));
}
```

When the current entity's vruntime has been adjusted by `reweight_eevdf()`, the `min_vruntime` of the cfs_rq may need to advance. Specifically, `reweight_eevdf()` adjusts `se->vruntime` based on the formula `v' = V - (V - v) * w / w'` to preserve the entity's lag. If the new weight `w'` is smaller than the old weight `w`, the entity's vruntime moves further from the average (increases if behind, decreases if ahead), potentially advancing the minimum. Without calling `update_min_vruntime()`, the `cfs_rq->min_vruntime` becomes stale and potentially behind where it should be.

Additionally, `__update_min_vruntime()` not only updates the `min_vruntime` field but also calls `avg_vruntime_update()` to adjust the `avg_vruntime` tracking, which is critical for the EEVDF eligibility checks. A stale `min_vruntime` means the `avg_vruntime` state also becomes inconsistent.

## Consequence

The immediate consequence is a stale `cfs_rq->min_vruntime` that can lag behind its correct value. This has several observable effects:

1. **Incorrect eligibility calculations**: The EEVDF scheduler uses `avg_vruntime` (which depends on `min_vruntime` through `avg_vruntime_update()`) to determine entity eligibility. A stale `min_vruntime` means the `avg_vruntime` accumulator is not properly maintained, potentially causing incorrect eligibility decisions. Entities that should be eligible for scheduling may be incorrectly deemed ineligible, or vice versa, leading to unfair CPU time distribution.

2. **Placement errors for newly waking tasks**: When a task wakes up, its vruntime is placed relative to `min_vruntime`. If `min_vruntime` is stale (too low), newly waking tasks may be placed too far back in virtual time, giving them an unfair advantage over existing runnable tasks. This can cause scheduling latency spikes for tasks that have been runnable for a while.

3. **Cumulative drift in group scheduling scenarios**: Since `update_cfs_group()` is called on every tick for the current entity via `entity_tick()`, and group entity weights change frequently as the load distribution across CPUs shifts, the missed `update_min_vruntime()` calls accumulate over time. In workloads with many cgroups and fluctuating load, the `min_vruntime` can fall significantly behind, causing increasingly skewed scheduling decisions. The effect is especially pronounced in environments with many task groups (e.g., container hosts) where group entity reweighting happens frequently.

While this bug does not cause crashes or kernel panics, it degrades scheduling fairness in EEVDF, particularly in cgroup-heavy workloads. Tasks in frequently-reweighted groups may experience subtle but persistent unfairness in CPU time allocation.

## Fix Summary

The fix moves the `update_min_vruntime()` call outside the `if (!curr)` block so that it is called unconditionally whenever the reweighted entity is on the runqueue:

```c
if (se->on_rq) {
    update_load_add(&cfs_rq->load, se->load.weight);
    if (!curr)
        __enqueue_entity(cfs_rq, se);

    update_min_vruntime(cfs_rq);
}
```

The `__enqueue_entity()` call remains guarded by `!curr` because only non-current entities need to be re-inserted into the RB-tree. The current entity is not in the RB-tree while it's running; it was dequeued (via `__dequeue_entity`) at the start of `reweight_entity()` only for non-current entities, and current entities skip RB-tree operations entirely since they are tracked separately via `cfs_rq->curr`.

The fix is correct and complete because `update_min_vruntime()` handles both cases properly: it checks `cfs_rq->curr` internally and considers the current entity's vruntime alongside the RB-tree's leftmost entity. Whether the reweighted entity is current or not, calling `update_min_vruntime()` ensures the rq-wide minimum is properly recalculated after the vruntime adjustment performed by `reweight_eevdf()`. The comment in the code is also moved to apply to the `update_min_vruntime()` call rather than the `__enqueue_entity()` call, clarifying that the rationale (stable `min_vruntime` needed during calculations, update at the end) applies to both the current and non-current cases.

## Triggering Conditions

The bug requires the following conditions to trigger:

1. **CONFIG_FAIR_GROUP_SCHED must be enabled**: The primary path to reweighting the current entity is through `update_cfs_group()` → `reweight_entity()`. Without fair group scheduling, `update_cfs_group()` is a no-op, and the only other caller, `reweight_task()` (from `set_user_nice()`), does not reweight the current entity in the same way. Group scheduling is enabled by default on most distributions.

2. **At least two CPUs**: Group entity weight computation via `calc_group_shares()` depends on the distribution of load across multiple per-CPU cfs_rqs belonging to the same task group. With a single CPU, the group entity's weight will be more static.

3. **A task group with changing load characteristics**: The group entity's weight (as computed by `calc_group_shares()`) must change between ticks. This happens when tasks are enqueued/dequeued from the group's per-CPU cfs_rqs, causing the proportional share calculation to produce a different result. A workload where tasks frequently wake up and block across different CPUs within the same cgroup will cause frequent weight changes.

4. **The currently running entity must be a group entity whose weight changes**: The bug specifically affects the `curr` entity path. The entity being reweighted must be `cfs_rq->curr` at the time `reweight_entity()` is called. This happens naturally on every scheduler tick through `entity_tick()` → `update_cfs_group(curr)`, where `curr` is the currently running group scheduling entity at that level of the hierarchy.

5. **The vruntime adjustment must cause a min_vruntime change**: The `reweight_eevdf()` function adjusts the entity's vruntime. For the missed `update_min_vruntime()` to have an observable effect, the adjusted vruntime of the current entity must be the new minimum (or affect the minimum calculation). This is more likely when the current entity has low vruntime (i.e., it was recently placed or is eligible) and its weight decreases (causing vruntime to shift further from the average).

The bug is deterministic once the conditions are met — it is not a race condition. Every tick where `update_cfs_group()` reweights the current group entity will miss the `update_min_vruntime()` call. In a multi-cgroup workload with fluctuating loads, this happens frequently.

## Reproduce Strategy (kSTEP)

The reproduction strategy uses kSTEP's cgroup support and task management to create a scenario where a group entity is repeatedly reweighted as the current entity, and then verifies that `min_vruntime` is properly updated.

### Setup

1. **QEMU configuration**: Use at least 2 CPUs. The driver runs on CPU 0, and task activity occurs on CPUs 1+.

2. **Create two cgroups**: Create cgroup "grpA" and cgroup "grpB" using `kstep_cgroup_create()`. Set both to run on CPU 1 using `kstep_cgroup_set_cpuset()`.

3. **Create tasks in grpA**: Create 2-3 CFS tasks and add them to "grpA" using `kstep_cgroup_add_task()`. Pin them to CPU 1.

4. **Create tasks in grpB**: Create 1 CFS task and add it to "grpB". Pin it to CPU 1. This task will serve as a load-generating companion to change the relative share.

5. **Create a "fluctuating" task**: Create an additional task in grpB that will be repeatedly blocked and woken. This causes the load (and hence `calc_group_shares()` result) to change for both groups' per-CPU entities.

### Execution Sequence

1. **Start all tasks**: Wake up all tasks so they are runnable on CPU 1. Tick a few times to let the scheduler settle and PELT load averages to build up.

2. **Record baseline state**: Use `KSYM_IMPORT` to access `cpu_rq(1)->cfs` (the top-level cfs_rq on CPU 1). Read `cfs_rq->min_vruntime` and the current entity's `se->vruntime`.

3. **Block the fluctuating task in grpB**: Call `kstep_task_block()` on the fluctuating task. This changes the load on CPU 1's grpB cfs_rq.

4. **Advance a tick**: Call `kstep_tick()`. During the tick, `entity_tick()` is called on the currently running group entity at the top-level cfs_rq. `update_cfs_group()` will detect that the group entity's weight has changed (because grpB's load changed) and call `reweight_entity()`.

5. **Check min_vruntime**: After the tick, read `cfs_rq->min_vruntime` from the parent cfs_rq (the one containing the group entities for grpA and grpB). On the buggy kernel, if the reweighted entity was `curr`, `min_vruntime` will not have been updated to reflect the adjusted vruntime. On the fixed kernel, it will be correctly updated.

6. **Wake the fluctuating task**: Call `kstep_task_wakeup()` on the blocked task. This changes the load again.

7. **Advance another tick**: Repeat the tick-and-check cycle.

### Detection Logic

The most reliable detection approach is to use the `on_tick_end` callback to inspect the state after each tick:

1. **Import internal symbols**: Use `KSYM_IMPORT` to import `update_min_vruntime`, or access internal structures directly via `internal.h` which gives access to `cpu_rq()`, `cfs_rq`, etc.

2. **Identify the parent cfs_rq**: Get the top-level CFS runqueue on CPU 1: `struct rq *rq1 = cpu_rq(1); struct cfs_rq *parent_cfs = &rq1->cfs;`

3. **Check if curr was reweighted**: In the `on_tick_end` callback, check if `parent_cfs->curr` exists and is a group entity (has `group_cfs_rq(parent_cfs->curr) != NULL`). Track the weight of the current group entity across ticks.

4. **Manually compute expected min_vruntime**: After a tick where the current group entity was reweighted (weight changed), manually compute what `min_vruntime` should be by looking at `curr->vruntime` and the leftmost entity in the RB-tree (`__pick_root_entity(parent_cfs)->min_vruntime`), taking the minimum. Compare this expected value with `parent_cfs->min_vruntime`.

5. **Pass/fail criteria**:
   - **Buggy kernel**: After reweighting `curr`, `cfs_rq->min_vruntime` will be stale — it won't reflect the potentially changed minimum due to the current entity's adjusted vruntime. The expected min_vruntime (computed manually) will differ from the actual `cfs_rq->min_vruntime`.
   - **Fixed kernel**: `cfs_rq->min_vruntime` will match the manually computed expected value because `update_min_vruntime()` was called.

### Alternative Simpler Approach

A simpler approach is to observe the `avg_vruntime` state, which is updated by `__update_min_vruntime()` (called within `update_min_vruntime()`):

1. Record `cfs_rq->avg_vruntime` and `cfs_rq->min_vruntime` before and after a tick where `curr` is reweighted.
2. On the buggy kernel, when `curr` is reweighted and its adjusted vruntime would advance `min_vruntime`, the `avg_vruntime` accumulator will not be updated via `avg_vruntime_update()`.
3. On the fixed kernel, both `min_vruntime` and `avg_vruntime` will be properly updated.

### kSTEP Changes Needed

No fundamental kSTEP changes are needed. The existing infrastructure provides:
- `kstep_cgroup_create()` and `kstep_cgroup_add_task()` for cgroup setup
- `kstep_task_create()`, `kstep_task_block()`, `kstep_task_wakeup()` for load fluctuation
- `on_tick_end` callback for post-tick inspection
- `internal.h` for access to `cpu_rq()`, `cfs_rq`, group entities
- `kstep_pass()`/`kstep_fail()` for result reporting

The driver should use `KSYM_IMPORT` if any non-exported symbols are needed for inspection, but all required state (`cfs_rq->min_vruntime`, `cfs_rq->curr`, `se->vruntime`, `se->load.weight`) is accessible through the internal scheduler headers already included by kSTEP.
