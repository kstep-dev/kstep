# EEVDF: Reweight entity placement bug causing unbounded lag drift

**Commit:** `6d71a9c6160479899ee744d2c6d6602a191deb1f`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.13
**Buggy since:** v6.7-rc2 (introduced by `eab03c23c2a1` "sched/eevdf: Fix vruntime adjustment on reweight")

## Bug Description

When a CFS scheduling entity is reweighted while on the runqueue (`se->on_rq == 1`), the old `reweight_eevdf()` function computes the entity's new vruntime directly from the scaled lag and the average vruntime: `se->vruntime = avruntime - vlag`. This computation is mathematically correct in isolation — the formula `v' = V - vl'` where `vl' = vl * w / w'` correctly preserves the lag through the weight change. However, it fails to account for a critical property of the EEVDF scheduler: when an entity is added back to the runqueue, the `place_entity()` function inflates the lag to compensate for the entity's own effect on the weighted average vruntime V.

The `place_entity()` function contains lag inflation logic that computes `vl_inflated = vl * (W + w_i) / W` where W is the total load of all other entities. This inflation is necessary because adding an entity with lag to the tree shifts V, which would reduce the entity's effective lag. Without this inflation, the entity's actual lag after placement is strictly less than the desired lag. The old `reweight_eevdf()` bypassed `place_entity()` entirely, computing the new vruntime without this inflation step, so the lag was systematically reduced (deflated) on every reweight cycle.

This bug is especially prominent with the DELAY_DEQUEUE feature (introduced in the "Complete EEVDF" patch set). With DELAY_DEQUEUE, entities remain on the runqueue even when they are logically dequeued, meaning the `se->on_rq` path in `reweight_entity()` is exercised much more frequently than before. In particular, group scheduling entities (created by cgroup hierarchies) are frequently reweighted via `update_cfs_group()` → `calc_group_shares()` → `reweight_entity()` while they remain on the runqueue. Each `update_cfs_group()` call triggers a weight transition, and each transition accumulates the lag error, causing the lag to drift unboundedly over time.

Peter Zijlstra discovered this bug by examining ftrace traces of `reweight_entity()`. He observed a weight transition cycle `1048576 → 2 → 1048576` where the lag should have been preserved (shot out and come back). With weights w=1048576 and an initial lag of −1123, transitioning to w'=2 should give `lag' = -1123 * 1048576 / 2 = -588775424`, and transitioning back should give `lag'' = -588775424 * 2 / 1048576 = -1123`. Instead, the trace showed the lag was −570951165 after the first transition (not −588775424) and −1049607 after the second (not −1123), with subsequent cycles diverging further.

## Root Cause

The root cause is that `reweight_eevdf()` duplicated the vruntime placement logic from `place_entity()` but did so incorrectly, omitting the critical lag inflation step.

In the buggy code path, when `se->on_rq` is true, `reweight_entity()` calls `reweight_eevdf(se, avruntime, weight)`. Inside `reweight_eevdf()`, the new vruntime is computed as:

```c
vlag = entity_lag(avruntime, se);          // vlag = clamp(V - v, -limit, limit)
vlag = div_s64(vlag * old_weight, weight); // scale lag by w/w'
se->vruntime = avruntime - vlag;           // v' = V - vl'
```

This sets `se->vruntime` to `V - vl'` where `vl'` is the scaled lag. However, the entity is still on the runqueue (it was dequeued from the RB-tree via `__dequeue_entity()` but remains logically on_rq). When it is re-enqueued via `__enqueue_entity()` at the end of `reweight_entity()`, no `place_entity()` call occurs — the entity is simply inserted back with the vruntime computed above.

The problem is that `place_entity()` contains the following inflation logic for entities with non-zero lag:

```c
load = cfs_rq->avg_load;
if (curr && curr->on_rq)
    load += scale_load_down(curr->load.weight);

lag *= load + scale_load_down(se->load.weight);
if (WARN_ON_ONCE(!load))
    load = 1;
lag = div_s64(lag, load);
```

This inflates the lag by `(W + w_i) / W` before computing `se->vruntime = vruntime - lag`. The inflation compensates for the fact that when the entity is added back to the tree, V shifts due to the entity's own weight, which would otherwise reduce the entity's effective lag. Without this inflation, each reweight cycle systematically under-compensates the lag.

Additionally, the deadline computation in `reweight_eevdf()` was also subtly wrong. It computed `se->deadline = avruntime + vslice` where `vslice = (d - V) * w / w'`. But `place_entity()` handles the relative deadline differently when `se->rel_deadline` is set — it adds the relative deadline to the newly-computed vruntime. The old code didn't use this mechanism, leading to inconsistent deadline placement.

The `entity_lag()` clamping also introduced additional error. The function clamps vlag to `[-limit, limit]` where `limit = calc_delta_fair(max(2*slice, TICK_NSEC), se)`. This clamping, while necessary to prevent runaway lag values, means that the computation `vlag_clamped * old_weight / weight` doesn't exactly preserve the lag through a round-trip weight transition, since the clamping is applied before scaling. The fix avoids this issue by using `update_entity_lag()` (which stores the clamped vlag in `se->vlag`) and then scaling `se->vlag` uniformly, which is the same path used for off-rq entities.

## Consequence

The primary observable consequence is unbounded lag drift for scheduling entities that are frequently reweighted. Since cgroup group entities are reweighted on every `update_cfs_group()` call (which happens on enqueue, dequeue, and `update_curr()`), the lag drift accumulates rapidly.

On a system with cgroup hierarchies and active task groups, this manifests as:

1. **Scheduling unfairness**: Entities with drifting lag receive increasingly incorrect amounts of CPU time. An entity whose lag drifts negative receives less CPU time than it should, while one with positive drift receives more. Over time, this can cause severe starvation of some task groups and over-scheduling of others.

2. **Vruntime divergence**: The vruntime of reweighted entities diverges from the average vruntime, which can cause entities to appear ineligible for scheduling (if vruntime is pushed far ahead of avg_vruntime) or to monopolize the CPU (if vruntime falls far behind). This disrupts the EEVDF fairness guarantees.

3. **Related dl_server crashes**: The mailing list discussion linked from the commit shows that this bug was discussed alongside a separate but related regression where `enqueue_dl_entity()` triggered a `WARN_ON_ONCE(!RB_EMPTY_NODE(&dl_se->rb_node))` warning, indicating a double-enqueue of the fair server's deadline entity. While the specific dl_server crash has a separate root cause, the lag drift from this bug contributes to the overall scheduler instability that makes such crashes more likely, as entities with wildly incorrect vruntimes and deadlines create inconsistent scheduler state.

4. **Performance degradation**: Tasks in affected cgroups experience unpredictable latency spikes and throughput drops. The commit message's trace shows lag values of −1049607 instead of the expected −1123 after a single weight round-trip — nearly 1000× the correct value. After multiple cycles, the divergence becomes astronomical.

## Fix Summary

The fix removes the `reweight_eevdf()` function entirely and restructures `reweight_entity()` to reuse `place_entity()` for the on_rq case, ensuring that the lag inflation logic is correctly applied.

In the new code, when `se->on_rq` is true, `reweight_entity()` does the following:
1. Calls `update_entity_lag(cfs_rq, se)` to compute `se->vlag = clamp(V - v, -limit, limit)`.
2. Converts the absolute deadline to a relative deadline: `se->deadline -= se->vruntime; se->rel_deadline = 1`.
3. Falls through to the unified scaling logic: `se->vlag = div_s64(se->vlag * se->load.weight, weight)` and `se->deadline = div_s64(se->deadline * se->load.weight, weight)` (only when `rel_deadline` is set).
4. After updating the load weight, calls `place_entity(cfs_rq, se, 0)` which: reads `se->vlag`, inflates it by `(W + w_i) / W`, computes `se->vruntime = V - inflated_lag`, then converts the relative deadline back to absolute: `se->deadline += se->vruntime`.

This approach unifies the on_rq and off_rq code paths. Both paths now scale `se->vlag` by `old_weight / new_weight`. The on_rq path additionally uses `place_entity()` to correctly handle re-insertion, while the off_rq path simply stores the scaled vlag for later use when the entity is eventually placed.

The fix also removes the `sched_feat(PLACE_REL_DEADLINE)` guard from the `if (se->rel_deadline)` check in `place_entity()`, making the relative deadline path unconditional. This is necessary because the reweight path sets `se->rel_deadline = 1` and depends on this branch being taken regardless of feature flags.

## Triggering Conditions

The bug is triggered whenever `reweight_entity()` is called on an entity that is currently on the runqueue (`se->on_rq == 1`). The most common scenarios are:

1. **Cgroup weight changes for group entities**: When tasks are in cgroup hierarchies, each task group has a group `sched_entity` whose weight is dynamically computed by `calc_group_shares()`. This function is called from `update_cfs_group()`, which is invoked during `enqueue_entity()`, `dequeue_entity()`, and `entity_tick()`. Any activity that changes the load distribution within a cgroup triggers reweighting of the group entity. With DELAY_DEQUEUE, group entities stay on the runqueue longer, making the on_rq path more frequent.

2. **Nice value changes via `set_user_nice()`**: When a task's nice value is changed, `reweight_task_fair()` is called, which calls `reweight_entity()` on the task's `sched_entity`. If the task is running or runnable (on_rq), this hits the buggy path. This can be triggered from userspace via `nice()`, `setpriority()`, or writing to `/proc/[pid]/autogroup`.

3. **Autogroup reweighting**: The autogroup mechanism automatically adjusts task group weights, calling `reweight_task_fair()` via `sched_autogroup_exit_task()` and `sched_autogroup_create_attach()`.

To observe the bug clearly, the following conditions help:

- **Multiple entities on the same cfs_rq**: The lag inflation factor `(W + w_i) / W` only matters when W > 0 (i.e., there are other entities). With only one entity, the lag is always zero and the bug has no effect.
- **Repeated weight transitions**: A single reweight introduces a small error. The bug becomes observable after multiple reweight cycles, as the error accumulates multiplicatively.
- **Large weight ratios**: Transitions between very different weights (e.g., high-priority cgroup weight 1048576 to low-priority weight 2) amplify the lag scaling, making the missing inflation more visible.
- **Kernel version v6.7-rc2 through v6.12.x**: The bug was introduced by commit `eab03c23c2a1` (v6.7-rc2) and fixed by this commit in v6.13. The bug becomes much more prominent in v6.12+ with the DELAY_DEQUEUE feature.
- **CONFIG_FAIR_GROUP_SCHED=y**: Required for cgroup-based reweighting of group entities.
- **At least 2 CPUs**: To avoid CPU 0 reservation by kSTEP driver.

## Reproduce Strategy (kSTEP)

The reproduction strategy uses cgroup weight changes to trigger repeated `reweight_entity()` calls on on_rq group entities, then observes the vlag drift.

### Setup Phase

1. **CPU Configuration**: Configure QEMU with at least 2 CPUs. All tasks will be pinned to CPU 1 (CPU 0 is reserved for the driver).

2. **Cgroup Creation**: Create 3 cgroups: `grp_target` (the group that will be reweighted), `grp_ref1`, and `grp_ref2` (reference groups for comparison). Set all groups to the same initial weight (e.g., 10000).

3. **Task Creation**: Create 3 tasks, one per cgroup. Pin all tasks to CPU 1. This ensures all three group scheduling entities are on the same `cfs_rq`.

### Run Phase

4. **Wake all tasks**: Call `kstep_task_wakeup()` on all three tasks so they're all on the runqueue competing for CPU 1.

5. **Stabilize vruntimes**: Run `kstep_tick_repeat(30)` to let the scheduler stabilize. After this, all three group entities should have roughly equal vruntimes and near-zero lag.

6. **Record baseline**: Read `se->vlag` and `se->vruntime` for the target group entity (`tasks[0]->se.parent`) and one reference entity (`tasks[1]->se.parent`). Record the target entity's vlag as `vlag_before`.

7. **Execute reweight cycles**: Perform N reweight cycles (e.g., N=10) on the target group. Each cycle:
   - Call `kstep_cgroup_set_weight("grp_target", 1)` to change weight to minimum.
   - Call `kstep_tick()` to let the scheduler process the change.
   - Call `kstep_cgroup_set_weight("grp_target", 10000)` to change weight back to original.
   - Call `kstep_tick()` to let the scheduler process the change.

   Each weight change triggers `update_cfs_group()` → `reweight_entity()` on the group entity. Since the entity is on_rq (DELAY_DEQUEUE keeps it there), this exercises the buggy `reweight_eevdf()` path on the buggy kernel, or the corrected `place_entity()` path on the fixed kernel.

8. **Record result**: After all cycles, read `se->vlag` for the target entity as `vlag_after`. Also read `se->vruntime` for both target and reference entities.

### Detection Phase

9. **Compare vlag drift**: On the fixed kernel, after a round-trip weight transition (w → w' → w), the vlag should be approximately preserved. After N cycles, `|vlag_after|` should be in the same order of magnitude as `|vlag_before|` (within a small tolerance for numerical rounding and scheduler activity).

   On the buggy kernel, each cycle accumulates an error because the lag inflation `(W + w_i) / W` is missing. With 3 entities of roughly equal weight, the inflation factor is approximately `(2W/3 + W/3) / (2W/3) = 1.5`. Missing this inflation on each half-cycle (dequeue+reweight) means the lag is multiplied by `1/1.5 = 0.667` per transition. Over N=10 full cycles (20 transitions), the lag would be reduced to `0.667^20 ≈ 0.003` of its original value — a ~300× reduction.

   However, the actual behavior is worse than simple reduction: the vruntime gets pushed in the wrong direction, causing the lag to grow unboundedly in the opposite direction. This is because `reweight_eevdf()` sets `se->vruntime = avruntime - vlag` without inflation, placing the entity closer to `avruntime` than it should be. Each cycle, the entity is placed progressively closer to (or past) `avruntime`, causing the vlag to flip sign and grow.

10. **Pass/fail criteria**: Compute `drift = |vruntime_target - vruntime_ref|`. On the fixed kernel, this should stay bounded (within a few slices, ~10ms). On the buggy kernel, after 10 reweight cycles with a large weight ratio, the drift should exceed 100ms or more. Use a threshold like 50ms of vruntime drift to determine pass/fail.

### Implementation Notes

- Access the group scheduling entity via `tasks[i]->se.parent` (requires cgroup hierarchy).
- Use `KSYM_IMPORT` if needed to access `avg_vruntime()` for diagnostic logging.
- Add `TRACE_INFO` logging before and after each reweight cycle to show the vlag and vruntime evolution.
- The `kstep_cgroup_set_weight()` API triggers the kernel's cgroup weight update path, which calls `cpu_shares_write_u64()` → `sched_group_set_shares()` → `update_cfs_group()` → `reweight_entity()`.
- Use `tick_interval_ns` at default (or slightly elevated) to ensure `update_cfs_group()` is triggered naturally during ticks via `entity_tick()` → `update_cfs_group()`.
- The `se->vlag` field can be read directly from the `sched_entity` structure via `internal.h`.
- For cleaner signal, pause all tasks except the target between reweight cycles so only the target entity is competing, then wake them back to observe the drift.

### Expected Behavior

- **Buggy kernel (pre-fix)**: The target entity's vlag and vruntime diverge significantly from reference entities after repeated reweight cycles. The drift grows with each cycle.
- **Fixed kernel (post-fix)**: The target entity's vlag and vruntime remain stable and close to reference entities even after many reweight cycles. The lag is correctly preserved through each weight transition.
