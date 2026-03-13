# EEVDF: Stale avg_vruntime (V) Used During Reweight of Non-Current Entity

**Commit:** `11b1b8bc2b98e21ddf47e08b56c21502c685b2c3`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.9-rc6
**Buggy since:** v6.7-rc2 (introduced by `eab03c23c2a1` "sched/eevdf: Fix vruntime adjustment on reweight")

## Bug Description

When a CFS scheduling entity is reweighted (i.e., its load weight changes due to a nice value change or task group weight recalculation), the EEVDF scheduler must adjust the entity's vruntime and virtual deadline to preserve fairness invariants. The `reweight_eevdf()` function performs these adjustments using the CFS run queue's weighted average virtual runtime V (`avg_vruntime(cfs_rq)`). These calculations require an accurate and up-to-date value of V to produce correct results.

The bug is that `reweight_entity()` only calls `update_curr(cfs_rq)` — which updates V by accounting for the current entity's recent execution — when the entity being reweighted is the currently running entity (`cfs_rq->curr == se`). When the entity being reweighted is **not** the current entity (i.e., it is a runnable-but-not-running entity in the RB tree), `update_curr()` is skipped. Instead, the code proceeds directly to `__dequeue_entity()`. This means `reweight_eevdf()` computes its vruntime and deadline adjustments based on a stale V that does not reflect the current entity's accumulated execution time since the last scheduler tick.

The practical impact is that the reweight calculations for non-current entities use an outdated V value. Since V is the weighted average vruntime of all runnable entities, and the current entity's vruntime has not been updated to include recent execution, V is behind where it should be. This causes incorrect vruntime and deadline adjustments, which in turn can lead to unfair scheduling: some entities may be given more or less CPU time than their weight entitles them to, and deadline-based task selection in EEVDF can make suboptimal choices.

## Root Cause

The root cause lies in the `reweight_entity()` function in `kernel/sched/fair.c`. Before the fix, the code structure was:

```c
if (se->on_rq) {
    /* commit outstanding execution time */
    if (curr)
        update_curr(cfs_rq);
    else
        __dequeue_entity(cfs_rq, se);
    update_load_sub(&cfs_rq->load, se->load.weight);
}
```

The `if (curr)` guard means `update_curr()` is only called when the entity being reweighted is the currently executing entity. The `update_curr()` function is critical because it:

1. Computes the delta execution time since the last update for the current entity.
2. Advances `cfs_rq->curr->vruntime` by the appropriate weighted amount.
3. Updates `cfs_rq->avg_vruntime` and `cfs_rq->min_vruntime` to reflect the new state.

When `se != cfs_rq->curr`, `update_curr()` is not called, so `cfs_rq->curr->vruntime` remains stale. Consequently, when `avg_vruntime(cfs_rq)` is later called inside `reweight_eevdf()`, it computes V using a stale vruntime for the current entity. The formula for V is:

```
V = min_vruntime + (Σ (v_i - v0) * w_i) / (Σ w_i)
```

Since `curr->vruntime` has not been updated, the numerator `avg_vruntime` field of `cfs_rq` is wrong — it under-counts the current entity's virtual runtime contribution. This makes V smaller than it should be.

The `reweight_eevdf()` function then uses this stale V (`avruntime`) in two critical calculations:

1. **Vruntime adjustment (vlag preservation):**
   ```c
   vlag = (s64)(avruntime - se->vruntime);
   vlag = div_s64(vlag * old_weight, weight);
   se->vruntime = avruntime - vlag;
   ```
   With a stale (too small) V, the computed vlag is incorrect, leading to a wrong adjusted vruntime for the entity.

2. **Deadline adjustment:**
   ```c
   vslice = (s64)(se->deadline - avruntime);
   vslice = div_s64(vslice * old_weight, weight);
   se->deadline = avruntime + vslice;
   ```
   Similarly, the deadline is adjusted relative to a stale V, producing an incorrect virtual deadline.

The severity of the error depends on how much execution time the current entity has accumulated since the last `update_curr()` call. If a reweight happens mid-tick (between scheduler ticks), the current entity may have run for a significant fraction of a tick without V being updated.

## Consequence

The primary consequence is **incorrect EEVDF scheduling fairness**. Since the vruntime and deadline of the reweighted entity are computed relative to a stale V, the entity ends up with a vruntime and deadline that do not correctly reflect its fair share. This can manifest as:

1. **Unfair CPU time distribution**: The reweighted entity may receive more or less CPU time than its new weight entitles it to, relative to other entities on the same CFS run queue. This violates the core EEVDF fairness guarantee that an entity's lag (the difference between its entitled and actual CPU time) is preserved through weight changes.

2. **Incorrect deadline ordering**: Since EEVDF uses virtual deadlines for task selection (picking the eligible entity with the earliest deadline), a wrongly computed deadline can cause the scheduler to pick tasks in a suboptimal order. An entity with an artificially early deadline might preempt other entities unfairly, or one with a late deadline might be starved.

3. **Performance degradation in cgroup-heavy workloads**: The `reweight_entity()` path is triggered whenever `update_cfs_group()` recalculates a task group's share on a particular CPU. This happens frequently — on every `update_curr()` call for group scheduling entities. In workloads with many cgroups and tasks that frequently become runnable/blocked, incorrect reweighting compounds over time, causing systematic unfairness. The benchmark results from K Prateek Nayak's testing on AMD EPYC systems showed that while the fix didn't cause regressions, some workloads (like schbench at low worker counts) showed measurable improvements, indicating the bug was affecting latency-sensitive scheduling decisions.

The bug does not cause kernel crashes, oopses, or data corruption — it is purely a fairness/correctness issue in the scheduler's virtual time accounting. However, in latency-sensitive or fairness-critical workloads (e.g., containers with CPU weight proportioning), the impact could be significant.

## Fix Summary

The fix is a two-line change that restructures `reweight_entity()` to always call `update_curr(cfs_rq)` when the entity is on the run queue (`se->on_rq`), regardless of whether it is the current entity:

```c
if (se->on_rq) {
    /* commit outstanding execution time */
    update_curr(cfs_rq);          // <-- now unconditional
    if (!curr)
        __dequeue_entity(cfs_rq, se);
    update_load_sub(&cfs_rq->load, se->load.weight);
}
```

Before the fix, `update_curr()` was inside `if (curr)` and `__dequeue_entity()` was in the `else` branch. After the fix, `update_curr()` is called first (unconditionally for any on-rq entity), and then `__dequeue_entity()` is called only for non-current entities. This ensures that when `reweight_eevdf()` later calls `avg_vruntime(cfs_rq)`, the current entity's vruntime has already been updated, so V is accurate.

This fix is correct because `update_curr()` is an idempotent-safe operation: calling it when the entity being reweighted is not the current entity simply updates the current entity's accounting (which is always valid to do). The only additional work is the small overhead of one extra `update_curr()` call per non-current reweight, which is negligible. The mathematical proofs in `reweight_eevdf()` (Corollary #1 and #2) rely on V being the true weighted average vruntime at the moment of calculation — this fix ensures that precondition holds.

Note that this commit is part of a two-patch series. The companion commit (`sched/eevdf: Fix miscalculation in reweight_entity() when se is not curr`) further fixes a related issue where V changes between the `update_curr()` call and the `avg_vruntime()` call inside `reweight_eevdf()` due to the `__dequeue_entity()` call removing the entity from the RB tree (which changes V). That second patch captures V before the dequeue and passes it explicitly to `reweight_eevdf()`.

## Triggering Conditions

The bug is triggered under the following conditions:

1. **Fair group scheduling must be enabled** (`CONFIG_FAIR_GROUP_SCHED=y`), which is the default on most distributions. This enables the `update_cfs_group()` path that calls `reweight_entity()` for group scheduling entities when their weight needs recalculation.

2. **At least two tasks must be runnable on the same CPU**, belonging to the same CFS run queue. One must be the currently running task (`cfs_rq->curr`) and the other must be a runnable-but-waiting task in the RB tree. The bug occurs when the non-current entity is reweighted.

3. **A reweight must occur for a non-current entity**. The most common path is `update_cfs_group()` → `reweight_entity()`, which is called during `enqueue_entity()`, `dequeue_entity()`, and `put_prev_entity()` for group scheduling entities. This happens when tasks within a task group enqueue or dequeue, causing the group's CFS share on a CPU to be recalculated.

4. **The current entity must have accumulated execution time since the last `update_curr()` call**. This is the key condition: the bigger the delta between the last `update_curr()` and the reweight event, the more stale V is, and the larger the miscalculation. In practice, this is common because `update_curr()` is typically called at tick boundaries, and reweight events can happen at any point in between (e.g., when a task wakes up and triggers group share recalculation).

5. **The entity being reweighted must be at a non-zero lag point** (`avruntime != se->vruntime`). If the entity happens to be at the exact avg_vruntime point, the vruntime adjustment is skipped (the `if (avruntime != se->vruntime)` guard), but the deadline adjustment is still affected.

The bug is **deterministic** — it always occurs whenever conditions 1-4 are met. It is not a race condition. The probability of hitting it is high in any workload with task groups and multiple runnable tasks, making it practically ubiquitous in production systems using cgroups for resource control (containers, systemd slices, etc.).

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by creating a scenario where a non-current CFS entity undergoes a reweight while V is stale.

### Setup

1. **Topology**: 2 CPUs minimum (CPU 0 reserved for driver, CPU 1 for the test). Configure with `kstep_topo_init()` and standard topology.

2. **Cgroups**: Create two cgroups (e.g., "grp_a" and "grp_b") with different weights. This ensures `CONFIG_FAIR_GROUP_SCHED` is active and group scheduling entities will be reweighted via `update_cfs_group()`.
   ```c
   kstep_cgroup_create("grp_a");
   kstep_cgroup_create("grp_b");
   kstep_cgroup_set_weight("grp_a", 1024);
   kstep_cgroup_set_weight("grp_b", 256);
   ```

3. **Tasks**: Create at least 2 CFS tasks and pin them to CPU 1:
   ```c
   struct task_struct *t1 = kstep_task_create();
   struct task_struct *t2 = kstep_task_create();
   kstep_task_pin(t1, 1, 1);
   kstep_task_pin(t2, 1, 1);
   kstep_cgroup_add_task("grp_a", t1->pid);
   kstep_cgroup_add_task("grp_b", t2->pid);
   ```

### Triggering the Bug

4. **Run tasks and let them accumulate vruntime**: Wake both tasks and advance several ticks so they establish non-zero vruntime and lag:
   ```c
   kstep_task_wakeup(t1);
   kstep_task_wakeup(t2);
   kstep_tick_repeat(20);
   ```

5. **Capture pre-reweight state**: Before triggering the reweight, use `KSYM_IMPORT(avg_vruntime)` to read V and the scheduling entities' vruntimes from the CFS run queue on CPU 1. Access `cpu_rq(1)->cfs` to get the CFS run queue, and iterate entities to find the non-current one.

6. **Trigger a reweight of the non-current entity**: Change the weight of one of the cgroups. When `update_cfs_group()` is called on the next scheduling event for that group's entities, it will call `reweight_entity()` for the group scheduling entity. The key is to change the weight **between ticks** — after some execution time has passed for the current entity without `update_curr()` being called:
   ```c
   kstep_cgroup_set_weight("grp_b", 512);  // triggers reweight
   kstep_tick();  // process the change
   ```

   Alternatively, waking a task in grp_b after blocking it can trigger `update_cfs_group()` during enqueue:
   ```c
   kstep_task_block(t2);
   kstep_tick_repeat(5);  // let t1 run and accumulate execution time
   kstep_task_wakeup(t2); // triggers enqueue -> update_cfs_group -> reweight_entity
   ```

### Detection

7. **Use `on_tick_begin` callback** to observe the state before and after the reweight. Import `avg_vruntime` and access the CFS run queue internals:
   ```c
   KSYM_IMPORT(avg_vruntime);
   ```

8. **Check V consistency**: The key detection is to read V before and after the reweight event. On the **buggy** kernel:
   - V will NOT reflect the current entity's execution time at the point `reweight_eevdf()` uses it.
   - The reweighted entity's vruntime will be adjusted based on a stale V.
   - After the reweight, the entity's lag (`V - se->vruntime`) scaled by its new weight should equal the lag scaled by its old weight. If V was stale, this invariant is violated.

9. **Pass/fail criteria**: Compare the entity's post-reweight vruntime against the expected value:
   ```
   expected_vruntime = V_accurate - (V_accurate - old_vruntime) * old_weight / new_weight
   ```
   Where `V_accurate` is the V computed after `update_curr()`. On the buggy kernel, the actual vruntime will differ from this expected value (using the stale V instead of V_accurate). On the fixed kernel, they should match.

   Concretely:
   - Record `V_before_reweight` by calling `avg_vruntime(cfs_rq)` (which internally reflects whether `update_curr` was called).
   - Record the entity's `se->vruntime` and `se->deadline` before and after the reweight.
   - On the buggy kernel: the lag (`V * w - v * w`) is NOT preserved through the reweight (because V was stale).
   - On the fixed kernel: the lag IS preserved.

   Use `kstep_pass()` if the weighted lag is preserved (within a small epsilon) and `kstep_fail()` otherwise.

### Alternative Strategy (Direct State Observation)

A simpler detection approach: After several ticks with both tasks running, block t2 and let t1 run for several more ticks. Then change grp_b's weight and wake t2. At wakeup time, capture V and the group scheduling entity's vruntime/deadline. Compare these values between buggy and fixed kernels. The difference should be observable because on the buggy kernel, the curr entity (t1's group SE) did not have its execution committed via `update_curr()` before V was sampled for the reweight.

### Expected Behavior

- **Buggy kernel**: After reweight of a non-current entity, the entity's adjusted vruntime and deadline will be computed from a stale V that is slightly behind the true V. The post-reweight lag will NOT equal `old_lag * old_weight / new_weight`. The error magnitude depends on how much execution time the current entity accumulated since the last `update_curr()`.

- **Fixed kernel**: `update_curr()` is called before `avg_vruntime()`, so V is accurate. The post-reweight vruntime and deadline correctly preserve the scaled lag invariant.

### kSTEP Framework Notes

- Access to CFS run queue internals is available via `#include "internal.h"` which includes `kernel/sched/sched.h`.
- `avg_vruntime()` can be imported with `KSYM_IMPORT(avg_vruntime)`.
- The `on_sched_group_alloc` callback can be used to observe task group creation.
- `cpu_rq(1)->cfs` gives direct access to the CFS run queue on CPU 1.
- Task group scheduling entities can be accessed via `tg->se[cpu]` and `tg->cfs_rq[cpu]`.
- No special hardware, NUMA, or I/O is needed — this is purely a virtual time accounting bug reproducible with basic CFS task and cgroup operations.
