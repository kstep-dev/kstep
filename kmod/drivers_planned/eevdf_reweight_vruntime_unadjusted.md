# EEVDF: Missing vruntime adjustment on reweight at non-zero-lag point

**Commit:** `eab03c23c2a162085b13200d7942fc5a00b5ccc8`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.7-rc2
**Buggy since:** v6.6-rc1 (introduced by `147f3efaa241` "sched/fair: Implement an EEVDF-like scheduling policy")

## Bug Description

When the EEVDF scheduling policy was introduced in Linux v6.6, the `reweight_entity()` function was updated to handle the new EEVDF-specific fields (`vlag`, `deadline`), but the code failed to adjust an on-rq entity's `vruntime` when its weight changed. In EEVDF, each entity's lag is defined as `lag_i = w_i * (V - v_i)`, where `w_i` is the entity's weight, `V` is the weighted average vruntime (`avg_vruntime`) of the cfs_rq, and `v_i` is the entity's vruntime. When a weight change occurs, the lag must be preserved: `lag' = w' * (V' - v')`. This requires recalculating the entity's vruntime because changing the weight alters the relationship between lag and vruntime.

The buggy code only adjusted the deadline for on-rq entities (scaling the relative deadline by the weight ratio), and only adjusted vlag for off-rq entities. On-rq entities at a non-zero-lag point (where `v != V`, i.e., the entity's vruntime differs from the cfs_rq's average vruntime) had their vruntime left unchanged, violating the EEVDF lag invariant. The mathematical proof in the commit message demonstrates that this is incorrect in all cases except when the entity is the sole entity on the cfs_rq.

Additionally, the buggy code used `avg_vruntime_sub()`/`avg_vruntime_add()` to temporarily remove and re-add the entity from the average vruntime tracking during reweight, but since the vruntime itself was not being adjusted, the entity was re-added at its old position. This meant the entity's position in the RB-tree (keyed by vruntime) was also stale, potentially corrupting the tree ordering and the cfs_rq's `min_vruntime` calculation.

The bug also affected the `update_cfs_group()` path, where group scheduling entities are periodically re-weighted based on the load of their group cfs_rq. Since `update_cfs_group()` is called from `entity_tick()`, `enqueue_entity()`, `dequeue_entity()`, and other hot paths, the incorrect vruntime persisted across many scheduling decisions, creating a systematic bias in scheduling fairness.

## Root Cause

The root cause is in the `reweight_entity()` function in `kernel/sched/fair.c`. Before the fix, when an on-rq entity was re-weighted, the code did:

```c
if (se->on_rq) {
    if (cfs_rq->curr == se)
        update_curr(cfs_rq);
    else
        avg_vruntime_sub(cfs_rq, se);
    update_load_sub(&cfs_rq->load, se->load.weight);
}
dequeue_load_avg(cfs_rq, se);
update_load_set(&se->load, weight);  // weight updated HERE

if (!se->on_rq) {
    se->vlag = div_s64(se->vlag * old_weight, weight);
} else {
    s64 deadline = se->deadline - se->vruntime;
    deadline = div_s64(deadline * old_weight, weight);
    se->deadline = se->vruntime + deadline;
    if (se != cfs_rq->curr)
        min_deadline_cb_propagate(&se->run_node, NULL);
}
```

For on-rq entities, the code correctly scaled the relative deadline `(d - v) * w/w'`, but completely neglected to adjust `se->vruntime`. The mathematical proof by Abel Wu shows why this is wrong: preserving lag through reweight requires `lag = (V - v) * w = (V' - v') * w'`, which gives `v' = V' - (V - v) * w / w'`. Since Corollary #2 proves that the average vruntime `V` does not change through reweight (i.e., `V' = V`), the formula simplifies to `v' = V - (V - v) * w / w'`.

A second problem was that the code used `avg_vruntime_sub()`/`avg_vruntime_add()` to handle non-current on-rq entities. These functions only update the `avg_vruntime_sum` and `avg_vruntime_n` accumulators that track the weighted sum for `avg_vruntime()` computation. They do **not** remove/re-insert the entity from the RB-tree. Since the fix adjusts `se->vruntime`, the entity's key in the RB-tree changes, requiring a proper `__dequeue_entity()`/`__enqueue_entity()` pair to maintain correct tree ordering.

A third subtle issue was the ordering of `update_load_set()`. In the buggy code, `update_load_set(&se->load, weight)` was called before the vlag/deadline adjustment, which meant `old_weight` had to be saved separately. The fix moved `update_load_set()` after the adjustments so that `se->load.weight` still holds the old weight when `reweight_eevdf()` reads it.

Finally, the `update_cfs_group()` function had an asymmetric early-return: on non-SMP builds, it returned early if `se->load.weight == shares`, avoiding the `reweight_entity()` call. On SMP builds, `reweight_entity()` was always called. The fix unified the behavior: both paths now check `se->load.weight != shares` before calling `reweight_entity()`.

## Consequence

The primary consequence is **unfair scheduling** among CFS tasks. When an entity is re-weighted (either through nice value changes via `reweight_task()`, or through periodic group weight updates via `update_cfs_group()`), the entity retains its old vruntime instead of the correctly adjusted value. This effectively changes the entity's lag in an uncontrolled manner:

- If the entity was "behind" (vruntime < avg_vruntime, positive lag), its lag becomes `w' * (V - v)` instead of the correct `w * (V - v)`. When weight increases (`w' > w`), the lag is artificially inflated; when weight decreases, the lag shrinks. This distorts the EEVDF fairness guarantee that each entity receives its proportional share of CPU time.
- For group scheduling entities (the most common path, since `update_cfs_group()` is called on every tick for task groups), the weight is continuously recalculated based on load. Each recalculation that happens at a non-zero-lag point compounds the error, creating a systematic drift in the entity's relative position.

A secondary consequence involves **RB-tree integrity**. Since the buggy code used `avg_vruntime_sub()`/`avg_vruntime_add()` instead of proper dequeue/enqueue, and since the fix needs to change `se->vruntime` (the RB-tree key), the tree could become misordered. The `min_deadline_cb_propagate()` call in the buggy code only updated the augmented deadline data, not the tree ordering by vruntime. While this might not cause an outright crash (since `min_vruntime` is computed from the leftmost node and traversal still works on a misordered tree), it could cause `pick_eevdf()` to select suboptimal entities or miss eligible ones.

The benchmark data in the LKML cover letter shows the fix had measurable but mixed impact across workloads (hackbench, netperf, tbench, schbench), suggesting the bug was actively causing scheduling decisions that differed from optimal EEVDF behavior. The most notable impact was on `netperf TCP_RR thread-96` which showed a +70% improvement with patch 1 alone, indicating the buggy reweight was causing significant unfairness in certain contended scenarios.

## Fix Summary

The fix introduces a new function `reweight_eevdf()` that properly adjusts both `vruntime` and `deadline` of an on-rq entity during reweight. The vruntime adjustment follows the mathematically derived formula: `v' = V - (V - v) * w / w'`, where `V = avg_vruntime(cfs_rq)`. The deadline adjustment uses a similar approach but anchored on `V` instead of `v`: `d' = V + (d - V) * w / w'`. Both formulas scale the entity's displacement from the average vruntime by the weight ratio, preserving the entity's lag proportionally.

The `reweight_entity()` function is restructured. For non-current on-rq entities, `__dequeue_entity(cfs_rq, se)` is called instead of `avg_vruntime_sub()`, which properly removes the entity from the RB-tree. After `reweight_eevdf()` adjusts the vruntime and deadline, `__enqueue_entity(cfs_rq, se)` re-inserts the entity at the correct RB-tree position based on its new vruntime, followed by `update_min_vruntime(cfs_rq)` to ensure the rq-wide minimum is up to date. The `update_load_set()` call is moved after the EEVDF adjustments so that `se->load.weight` still contains the old weight during the calculations.

For off-rq entities, the existing vlag scaling (`se->vlag = div_s64(se->vlag * se->load.weight, weight)`) is retained, since these entities are not in the tree and their vruntime will be recomputed at enqueue time from their vlag. The `update_cfs_group()` function is cleaned up to consistently check `se->load.weight != shares` before calling `reweight_entity()` on both SMP and non-SMP builds.

## Triggering Conditions

The bug is triggered whenever a CFS entity is re-weighted while it is on the runqueue and its vruntime differs from the cfs_rq's `avg_vruntime`. The specific conditions are:

1. **Task group scheduling (CONFIG_FAIR_GROUP_SCHED)**: This is the most common trigger path. When tasks are in a cgroup (task group), the group entity's weight is periodically recalculated by `update_cfs_group()` → `calc_group_shares()` → `reweight_entity()`. This happens from `entity_tick()` on every scheduler tick, from `enqueue_entity()` on wakeup, and from `dequeue_entity()` on sleep. The group entity's weight changes whenever the load on the group cfs_rq changes (e.g., tasks wake up or sleep within the group), which is extremely common in any workload with task groups.

2. **Nice value changes (reweight_task)**: When a task's nice value is changed via `set_user_nice()` or `sched_setattr()`, `reweight_task()` calls `reweight_entity()` directly on the task's sched_entity. If the task is currently on the runqueue and not the current task, the bug manifests.

3. **Non-zero lag requirement**: The entity must have `vruntime != avg_vruntime(cfs_rq)`. This is true for virtually all entities unless the entity is the only one on the cfs_rq (in which case its vruntime equals the average). With 2 or more runnable entities, the condition is satisfied.

4. **Multiple runnable entities**: At least 2 CFS entities must be runnable on the same cfs_rq for the entity being reweighted to have a non-zero lag. More entities increase the likelihood and magnitude of the lag discrepancy.

5. **Kernel configuration**: `CONFIG_FAIR_GROUP_SCHED` must be enabled for the group scheduling path (enabled by default on most distributions). For the `reweight_task()` path, no special configuration is needed.

The bug is highly deterministic and triggers on every reweight operation at a non-zero-lag point. It is not a race condition — it is a missing computation that always produces the wrong result. The severity scales with the magnitude of the weight change and the entity's lag at the time of reweight.

## Reproduce Strategy (kSTEP)

The reproduction strategy focuses on demonstrating that an on-rq entity's vruntime is NOT adjusted during reweight on the buggy kernel, while it IS adjusted on the fixed kernel. We use the `reweight_task()` path (nice value change) since it provides the most direct control.

### Setup

1. **QEMU configuration**: 2 CPUs minimum. No special topology needed.
2. **Tasks**: Create 3 CFS tasks pinned to CPU 1 (avoid CPU 0 which runs the driver). All start with the same default nice value (0, weight 1024).
3. **Allow tasks to run**: Wake up all 3 tasks and let them run for several ticks to establish non-zero lag values. After some ticks, different tasks will have accumulated different amounts of runtime, creating varying vruntimes.

### Trigger

4. **Record state before reweight**: Read `avg_vruntime(cfs_rq)` (the `V` value) and the first task's `se->vruntime` (the `v` value) and `se->load.weight` (the old weight `w`) from the cfs_rq on CPU 1. Use `KSYM_IMPORT(avg_vruntime)` to access the function.
5. **Pause one non-target task**: Pause one of the other tasks to ensure the target task is NOT the current task (since the current task path just calls `update_curr()` and the vruntime adjustment works differently). Alternatively, use `kstep_task_kernel_pause()` on the target task to block it, then wake it up so it's enqueued but not current, then change nice.
6. **Change nice value**: Call `kstep_task_set_prio(task1, new_nice)` to change the first task's nice value (e.g., from 0 to 5, which changes weight from 1024 to 335). This calls `set_user_nice()` → `reweight_task()` → `reweight_entity()`.
7. **Record state after reweight**: Read the same fields again.

### Verification

8. **Compute expected vruntime**: Using the pre-reweight values, compute `expected_v' = V - (V - v) * w / w'` where `w = 1024` (nice 0) and `w' = 335` (nice 5).
9. **Compare**:
   - **Buggy kernel**: `se->vruntime` after reweight equals the old vruntime (unchanged). The lag `(V - v) * w` is NOT equal to `(V - v') * w'` because `v' = v` and `w' != w`. Report `kstep_fail()`.
   - **Fixed kernel**: `se->vruntime` after reweight equals the expected value `V - (V - v) * w / w'`. The lag is preserved. Report `kstep_pass()`.

### Implementation Details

The driver should:
- Use `KSYM_IMPORT(avg_vruntime)` to import the `avg_vruntime()` function for reading the cfs_rq average.
- Access the cfs_rq via `cpu_rq(1)->cfs` for the root cfs_rq, or via `se->cfs_rq` for task entities.
- Read `se->vruntime`, `se->deadline`, `se->load.weight` before and after the nice value change.
- Compute the expected adjusted vruntime and compare with actual.
- Log all values (old vruntime, new vruntime, expected vruntime, avg_vruntime, old weight, new weight, old lag, new lag) for debugging.
- Use `kstep_task_set_prio()` to trigger the reweight. The nice-to-prio mapping is: `kstep_task_set_prio(p, 120 + nice)` where default is 120 (nice 0).
- Additionally, verify the deadline adjustment: compute `expected_d' = V + (d - V) * w / w'` and compare with actual `se->deadline`.

### Alternative Strategy: Group Entity Reweight

A second approach uses `update_cfs_group()`:
1. Create a cgroup with `kstep_cgroup_create("grp")`.
2. Place 2 tasks in the cgroup, pin both to CPU 1.
3. Create 1 task outside the cgroup on CPU 1 (so the group entity competes with another entity, ensuring non-zero lag).
4. Let tasks run for several ticks.
5. Change the cgroup weight with `kstep_cgroup_set_weight("grp", new_weight)`. On the next `update_cfs_group()` call (triggered by the next tick or enqueue/dequeue), the group entity's weight will be recalculated.
6. Observe the group entity's vruntime before and after.

This approach tests the more common `update_cfs_group()` path but is harder to control precisely because the exact weight depends on `calc_group_shares()`. The direct `reweight_task()` path is recommended for the primary test.

### Pass/Fail Criteria

- **Pass**: After reweight, the entity's vruntime changes according to the formula `v' = V - (V - v) * w / w'` (with reasonable tolerance for integer division rounding). The entity's lag `(V - v') * w'` approximately equals the original lag `(V - v) * w`.
- **Fail**: After reweight, the entity's vruntime is unchanged (still equals old `v`), meaning the lag changed by a factor of `w'/w`.

The tolerance for comparison should account for integer division truncation (±1 unit of vruntime per division). A mismatch larger than 2 units between expected and actual vruntime indicates the bug is present.
