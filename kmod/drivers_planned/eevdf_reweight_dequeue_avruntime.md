# EEVDF: Miscalculated avg_vruntime in reweight_eevdf() after __dequeue_entity()

**Commit:** `afae8002b4fd3560c8f5f1567f3c3202c30a70fa`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.9-rc6
**Buggy since:** v6.7-rc2 (introduced by `eab03c23c2a1` "sched/eevdf: Fix vruntime adjustment on reweight")

## Bug Description

When a CFS scheduling entity is reweighted while it is on the runqueue (`se->on_rq == 1`) and it is NOT the currently running task (`se != cfs_rq->curr`), the `reweight_entity()` function first dequeues the entity from the RB tree via `__dequeue_entity(cfs_rq, se)` before calling `reweight_eevdf(cfs_rq, se, weight)`. The `__dequeue_entity()` call internally invokes `avg_vruntime_sub(cfs_rq, se)`, which removes the entity's contribution from the CFS runqueue's weighted average vruntime tracking fields (`cfs_rq->avg_vruntime` and `cfs_rq->avg_load`). This causes the subsequent `avg_vruntime(cfs_rq)` call inside `reweight_eevdf()` to return a different value of V (the weighted average vruntime) than the V that existed before the dequeue — specifically, V computed without including the entity being reweighted.

The EEVDF reweight formulas in `reweight_eevdf()` are mathematically derived under the assumption that V includes the entity being reweighted. The key formula `v' = V - (V - v) * w / w'` (new vruntime from old vruntime, preserving lag) uses V as the weighted average of ALL entities including the one being reweighted. When V is instead computed without the entity (because it was already dequeued from the tree), the resulting vruntime and deadline are incorrect.

This bug was discovered by Tianchen Ding while instrumenting `reweight_entity()` with logging. He observed that `avg_vruntime(cfs_rq)` returned different values before and after `__dequeue_entity()`, which meant the `curr == se` and `curr != se` paths produced different results for the same logical operation. Abel Wu confirmed this was a bug: since the entity is still logically on_rq, it should be included when computing V, but `__dequeue_entity()` removes its contribution. The fix, which Tianchen proposed and Abel endorsed, is to capture `avg_vruntime(cfs_rq)` BEFORE calling `__dequeue_entity()` and pass this pre-dequeue V to `reweight_eevdf()`.

This commit is the second of a two-patch series. The first patch (commit `11b1b8bc2b98` "sched/eevdf: Always update V if se->on_rq when reweighting") fixes a related issue: in the original code, `update_curr()` was only called when `se == curr`, meaning V was not up-to-date when `se != curr`. The companion fix makes `update_curr()` unconditional so that curr's outstanding execution time is committed before V is captured, ensuring an accurate V regardless of which entity is being reweighted.

## Root Cause

The root cause is an ordering issue in `reweight_entity()` where the entity is removed from the CFS RB tree BEFORE the average vruntime V is captured for the EEVDF reweight calculation.

In the buggy code path, when `se->on_rq == 1` and `se != cfs_rq->curr`, `reweight_entity()` executes:

```c
if (se->on_rq) {
    if (curr)
        update_curr(cfs_rq);      // only called for curr
    else
        __dequeue_entity(cfs_rq, se);  // removes se from RB tree, changes V
    update_load_sub(&cfs_rq->load, se->load.weight);
}
// ...
if (!se->on_rq) {
    se->vlag = div_s64(se->vlag * se->load.weight, weight);
} else {
    reweight_eevdf(cfs_rq, se, weight);  // uses avg_vruntime() which is now WRONG
}
```

Inside `__dequeue_entity()`, `avg_vruntime_sub(cfs_rq, se)` is called, which modifies `cfs_rq->avg_vruntime` by subtracting `se->load.weight * (se->vruntime - cfs_rq->min_vruntime)` and modifies `cfs_rq->avg_load` by subtracting `se->load.weight`. After this point, any call to `avg_vruntime(cfs_rq)` computes V without including `se`.

Then `reweight_eevdf()` is called, and inside it:

```c
static void reweight_eevdf(struct cfs_rq *cfs_rq, struct sched_entity *se,
                           unsigned long weight)
{
    unsigned long old_weight = se->load.weight;
    u64 avruntime = avg_vruntime(cfs_rq);  // BUG: V does not include se
    s64 vlag, vslice;

    // VRUNTIME: v' = V - (V - v) * w / w'
    if (avruntime != se->vruntime) {
        vlag = (s64)(avruntime - se->vruntime);
        vlag = div_s64(vlag * old_weight, weight);
        se->vruntime = avruntime - vlag;
    }

    // DEADLINE: d' = V + (d - V) * w / w'
    vslice = (s64)(se->deadline - avruntime);
    vslice = div_s64(vslice * old_weight, weight);
    se->deadline = avruntime + vslice;
}
```

Here, `avruntime` is the weighted average vruntime of all entities EXCEPT `se`. Call this `V_without_se`. The correct value should be `V_with_se` — the weighted average including `se`. These two values differ by exactly:

```
V_with_se = (W_rest * V_without_se + w_se * v_se) / (W_rest + w_se)
```

where `W_rest = cfs_rq->avg_load` (after dequeue), `w_se = se->load.weight`, and `v_se = se->vruntime`. The difference `V_with_se - V_without_se = w_se * (v_se - V_without_se) / (W_rest + w_se)`, which is nonzero whenever the entity has nonzero lag (i.e., `v_se != V_with_se`, equivalently `v_se != V_without_se` in the general multi-entity case).

Using the wrong V in the formula `v' = V - (V - v) * w / w'` produces an incorrect new vruntime. Specifically, with V_wrong = V_without_se instead of V_correct = V_with_se:

```
v'_buggy  = V_wrong  - (V_wrong  - v) * w / w'
v'_correct = V_correct - (V_correct - v) * w / w'
```

The error in the new vruntime is:
```
v'_buggy - v'_correct = (V_wrong - V_correct) * (1 - w/w')
```

This error is proportional to (a) how much V shifts when the entity is dequeued, and (b) the weight change ratio. For large weight changes and entities with significant lag, this error can be substantial. The same error applies to the deadline calculation.

Note that when `se == cfs_rq->curr`, this bug does NOT occur because the `curr` entity is not part of the RB tree in the first place (it was removed when it was picked to run), so `avg_vruntime(cfs_rq)` naturally excludes it already, and the formulas account for this. The asymmetry between the curr and non-curr paths is precisely the source of the bug.

## Consequence

The primary consequence is incorrect vruntime and deadline values for scheduling entities that are reweighted while on the runqueue but not currently running. This manifests as a violation of EEVDF's lag preservation invariant during reweight operations.

Specifically, the EEVDF property that "reweight does NOT affect the weighted average vruntime of all entities" (COROLLARY #2 in the kernel comments) relies on the entity's vruntime being correctly adjusted. When the vruntime adjustment uses the wrong V, the entity's lag after reweight is not the correctly scaled version of its lag before reweight. For example, if an entity has lag = (V - v) * w = L before reweight, the correct new lag should be L (preserved), yielding `new_v = V - L/w'`. But the buggy code computes `new_v = V' - (V' - v) * w / w'` where V' ≠ V, resulting in a different effective lag.

In practice, this bug affects group scheduling entities most frequently, because they are reweighted on every `update_cfs_group()` call (which happens during `enqueue_entity()`, `dequeue_entity()`, and `entity_tick()` via `update_curr()`). A group entity's weight is dynamically recalculated by `calc_group_shares()` based on the group's load relative to the system. When the group entity is on the cfs_rq of the parent but is not the currently running entity, each reweight introduces a small vruntime error. Over many reweight cycles, these errors accumulate, causing the group entity's vruntime and deadline to drift from their correct values. This leads to scheduling unfairness: the affected task group may receive more or less CPU time than it should, depending on the direction of drift. Tasks within the cgroup experience unpredictable latency and throughput variations.

The severity depends on the number of entities on the cfs_rq and the magnitude of weight changes. With only two entities and small weight changes, the error per reweight is small. With many entities and large weight swings (e.g., a heavily loaded system with dynamic cgroup weights), the drift can become significant. There is no crash or kernel panic — the bug is a silent correctness issue that degrades scheduling fairness.

## Fix Summary

The fix changes the interface of `reweight_eevdf()` to accept the pre-computed average vruntime `avruntime` as a parameter instead of computing it internally from the (already modified) `cfs_rq` state.

In the fixed `reweight_entity()`, the sequence becomes:

```c
if (se->on_rq) {
    update_curr(cfs_rq);                    // commit curr's execution (unconditional, from companion fix)
    avruntime = avg_vruntime(cfs_rq);       // capture V BEFORE dequeue
    if (!curr)
        __dequeue_entity(cfs_rq, se);       // now safe to dequeue
    update_load_sub(&cfs_rq->load, se->load.weight);
}
// ...
if (se->on_rq) {
    reweight_eevdf(se, avruntime, weight);  // use pre-dequeue V
} else {
    se->vlag = div_s64(se->vlag * se->load.weight, weight);
}
```

The signature of `reweight_eevdf()` is changed from `reweight_eevdf(struct cfs_rq *cfs_rq, struct sched_entity *se, unsigned long weight)` to `reweight_eevdf(struct sched_entity *se, u64 avruntime, unsigned long weight)`. It no longer needs the `cfs_rq` pointer at all since V is passed in directly.

Peter Zijlstra also flipped the `if/else` condition for clarity: the original code tested `if (!se->on_rq) { ... } else { reweight_eevdf(); }` and the fix changes it to `if (se->on_rq) { reweight_eevdf(); } else { ... }`. This is a cosmetic change that makes the code more readable by putting the on_rq (more complex) case first.

This fix is correct and complete because it ensures that the V used in the EEVDF formulas always reflects the state where the entity being reweighted is still logically part of the runqueue, matching the mathematical assumptions of the derivation. When `se == curr`, V already excludes curr (since curr is not in the RB tree), and the formula correctly accounts for this. When `se != curr`, V is now captured before dequeue, so it correctly includes `se`.

## Triggering Conditions

The bug is triggered when ALL of the following conditions hold simultaneously:

1. **The entity `se` is on the runqueue**: `se->on_rq == 1`. This means the entity is either running or runnable (waiting in the CFS RB tree).

2. **The entity is NOT the currently running task**: `se != cfs_rq->curr`. This causes `reweight_entity()` to take the `__dequeue_entity()` path (removing `se` from the tree before reweight) rather than the `update_curr()` path.

3. **The entity has nonzero lag**: `avg_vruntime(cfs_rq) != se->vruntime`. If the entity happens to be exactly at the average vruntime (zero lag), the dequeue does not change V from the entity's perspective (the `if (avruntime != se->vruntime)` check in `reweight_eevdf()` skips the vruntime adjustment), so the bug has no effect. In practice, entities almost always have nonzero lag.

4. **There are other entities on the cfs_rq**: If `se` is the only entity, dequeuing it makes the cfs_rq empty, and the V computation is degenerate. With multiple entities, removing one visibly shifts V.

5. **A reweight operation is triggered**: The most common triggers are:
   - `update_cfs_group()` → `reweight_entity()`: Called during `enqueue_entity()`, `dequeue_entity()`, and `entity_tick()` (via `update_curr()`) for group scheduling entities. Requires `CONFIG_FAIR_GROUP_SCHED=y`.
   - `reweight_task()` → `reweight_entity()`: Called when a task's nice value is changed via `set_user_nice()`, triggered from userspace `nice()` or `setpriority()` syscalls.

6. **The weight actually changes**: `update_cfs_group()` contains a check `if (unlikely(se->load.weight != shares))` before calling `reweight_entity()`. The weight must differ from the current value.

The most reliable reproduction scenario uses cgroup group scheduling. In a multi-level cgroup hierarchy, the group entity at each level is reweighted dynamically as the load distribution changes. The group entity is typically NOT curr (the currently running entity on the parent cfs_rq is usually a different group entity or a direct task). Multiple tasks joining, leaving, or changing their execution patterns within the cgroup cause `calc_group_shares()` to recompute the group entity's weight, triggering `reweight_entity()`.

The bug is deterministic once the conditions are met — it does not require any race condition or specific timing. Every call to `reweight_entity()` on a non-curr on_rq entity with nonzero lag produces an incorrect result. The error magnitude depends on the entity's lag, the weight change ratio, and the number of other entities on the cfs_rq.

## Reproduce Strategy (kSTEP)

The reproduction strategy creates a cgroup hierarchy that triggers `reweight_entity()` on group scheduling entities and observes the incorrect vruntime calculation by comparing the entity's lag before and after the reweight operation.

### Setup

1. **QEMU configuration**: At least 2 CPUs (CPU 0 is reserved for the driver, CPU 1 for the test workload). `CONFIG_FAIR_GROUP_SCHED=y` must be enabled in the kernel config (it is enabled by default in most configs).

2. **Create a test cgroup**: Use `kstep_cgroup_create("test")` to create a cgroup. Set its initial weight with `kstep_cgroup_set_weight("test", 1024)` (default nice-0 weight).

3. **Create tasks**: Create 3 CFS tasks using `kstep_task_create()`. Pin all 3 to CPU 1 using `kstep_task_pin(p, 1, 2)`. Add 2 of them to the "test" cgroup using `kstep_cgroup_add_task("test", task_pid_nr(p))`. Leave the third task in the root cgroup. This creates a cgroup hierarchy where the "test" group entity on CPU 1's root cfs_rq will have its weight dynamically computed by `calc_group_shares()`.

4. **Warm up**: Run `kstep_tick_repeat(100)` to let the tasks accumulate execution time and establish nonzero lag values for the group entity.

### Triggering the Bug

5. **Read pre-reweight state**: After warmup, access the group scheduling entity for the "test" cgroup on CPU 1 using internal scheduler structures. Use `KSYM_IMPORT` to get the task group pointer, then access `tg->cfs_rq[1]->tg->se[1]` to get the group sched_entity. Record:
   - `se->vruntime` (entity's virtual runtime)
   - `se->deadline` (entity's virtual deadline)
   - `se->load.weight` (current weight)
   - `avg_vruntime(cfs_rq_of(se))` (current V on the parent cfs_rq)
   - Compute `lag_before = (s64)(V - se->vruntime) * se->load.weight`

6. **Trigger reweight**: Change the cgroup weight using `kstep_cgroup_set_weight("test", 512)` (or any different weight). This updates `tg->shares`. Then trigger a tick with `kstep_tick()` which calls `entity_tick()` → `update_curr()` → `update_cfs_group()` → `reweight_entity()` on the group entity. The group entity will typically NOT be curr on the parent cfs_rq (another task or group entity will be running), so the buggy non-curr path is taken.

7. **Read post-reweight state**: After the tick, read:
   - `se->vruntime` (new vruntime)
   - `se->deadline` (new deadline)
   - `se->load.weight` (new weight, should reflect the changed shares)
   - `avg_vruntime(cfs_rq_of(se))` (new V)
   - Compute `lag_after = (s64)(V_new - se->vruntime_new) * se->load.weight_new`

### Detection

8. **Verify lag preservation**: According to the EEVDF reweight invariant, `lag_after` should equal `lag_before`. On the **buggy kernel**, `lag_after != lag_before` because the wrong V was used in the calculation. The magnitude of the difference depends on the entity's lag and the number of entities on the cfs_rq. On the **fixed kernel**, `lag_after == lag_before` (within rounding tolerance of integer division).

9. **Alternative detection via V preservation**: Record `V_before` (avg_vruntime on the parent cfs_rq before reweight) and `V_after` (after reweight). According to COROLLARY #2, V should not change during reweight. On the buggy kernel, V may visibly shift after the reweight+re-enqueue cycle because the entity's incorrect vruntime propagates through `avg_vruntime_add()` when re-enqueued.

### Callbacks

Use the `on_tick_begin` or `on_tick_end` callback to instrument the state before and after each tick. Alternatively, use the `on_sched_group_alloc` callback if available, or simply read state between explicit `kstep_tick()` calls in the `run()` function.

### Pass/Fail Criteria

- **PASS (fixed kernel)**: `|lag_after - lag_before| < tolerance` (e.g., tolerance of 1024 to account for integer division rounding) across multiple reweight cycles.
- **FAIL (buggy kernel)**: `|lag_after - lag_before| > tolerance`, with the error growing over repeated reweight cycles. The direction and magnitude of the error depend on the sign of the lag and the weight change direction.

### Implementation Notes

- Access internal structures via `#include "internal.h"` which provides `cpu_rq()`, `cfs_rq`, and all `kernel/sched/sched.h` internals.
- Use `KSYM_IMPORT(avg_vruntime)` to import the `avg_vruntime()` function if it is not directly accessible.
- The group sched_entity can be accessed via `tg->se[cpu]` where `tg` is the task_group pointer. The cgroup's `task_group` can be found from the cgroup filesystem or by iterating task groups.
- Ensure the group entity is NOT curr when the reweight happens. This can be ensured by having a task from a different cgroup (or the root cgroup) be the currently running task on the target CPU when the tick fires. Having the third (root-cgroup) task running at tick time achieves this.
- The companion bug (lack of `update_curr()` for non-curr) is also present on the buggy kernel (pre `11b1b8bc2b98`), which adds additional inaccuracy to V. Both bugs are fixed in the same patch series.
- Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,7,0) && LINUX_VERSION_CODE < KERNEL_VERSION(6,9,0)` to match the affected version range (buggy since v6.7-rc2, fixed in v6.9-rc6).
