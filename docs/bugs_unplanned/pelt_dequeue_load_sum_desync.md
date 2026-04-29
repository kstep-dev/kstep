# PELT: dequeue_load_avg load_sum/load_avg Desynchronization

**Commit:** `ceb6ba45dc8074d2a1ec1117463dc94a20d4203d`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.14-rc1
**Buggy since:** pre-v5.13 (the inconsistent subtraction logic in `dequeue_load_avg()` existed since the PELT load tracking rework; the SCHED_WARN_ON that detects the inconsistency was added in v5.14-rc1 by commit `9e077b52d86a`)

## Bug Description

The Per-Entity Load Tracking (PELT) subsystem in the Linux CFS scheduler maintains two related values for each CFS run queue: `load_avg` and `load_sum`. These values are connected through the PELT divider: conceptually, `load_avg ≈ load_sum / divider`, where the divider is computed as `PELT_MIN_DIVIDER + period_contrib`. The PELT divider captures the geometric series decay and period contribution, and keeping `load_sum` and `load_avg` in sync is essential for correct load tracking and scheduling decisions.

When a scheduling entity is dequeued from a CFS run queue, the function `dequeue_load_avg()` is responsible for subtracting the entity's contribution from the CFS run queue's aggregate PELT load values. Prior to the fix, this function independently subtracted `se->avg.load_avg` from `cfs_rq->avg.load_avg` and `se_weight(se) * se->avg.load_sum` from `cfs_rq->avg.load_sum`, each using the `sub_positive()` macro which clamps the result to zero on underflow. This independent subtraction could leave `load_sum` and `load_avg` out of sync.

The problem arises because the `sub_positive()` macro clamps independently on each value. Due to rounding, PELT divider drift (from `period_contrib` updates), and the differing formulas for the two subtractions (one involves `se_weight()` multiplication), it is possible for `load_sum` to be clamped to zero while `load_avg` remains positive, or for the ratio `load_sum / load_avg` to diverge from the expected divider value. This violates the fundamental PELT invariant.

A SCHED_WARN_ON check added in commit `9e077b52d86a` ("sched/pelt: Check that *_avg are null when *_sum are") in the `cfs_rq_is_decayed()` function detected this inconsistency: it warns when `load_avg`, `util_avg`, or `runnable_avg` is non-zero while the corresponding `_sum` value is zero. A prior fix (commit `1c35b07e6d39`) addressed the same class of inconsistency in `update_cfs_rq_load_avg()` for the "removed" entity subtraction path, but missed the `dequeue_load_avg()` path, which is the path this commit fixes.

## Root Cause

The root cause lies in the `dequeue_load_avg()` function in `kernel/sched/fair.c`. Before the fix, the function was:

```c
static inline void
dequeue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    sub_positive(&cfs_rq->avg.load_avg, se->avg.load_avg);
    sub_positive(&cfs_rq->avg.load_sum, se_weight(se) * se->avg.load_sum);
}
```

The `sub_positive()` macro performs a saturating subtraction: it subtracts `val` from `*ptr` and clamps the result to zero if underflow would occur. The two subtractions operate on different scales: `load_avg` is a weighted average, while `load_sum` is `se_weight(se) * se->avg.load_sum`, which encodes the weight-scaled geometric sum.

The fundamental issue is that `load_sum` and `load_avg` are not perfectly in sync at all times. The PELT divider (`get_pelt_divider(&avg)` = `PELT_MIN_DIVIDER + avg->period_contrib`) increases whenever `period_contrib` is updated during PELT decay computations, but the `_sum` and `_avg` values are not always updated atomically in perfect lockstep. Specifically, when PELT advances periods and updates `period_contrib`, this changes the divider but does not immediately recalculate all `_sum` values to maintain `_sum == _avg * divider` exactly. This means there can be small discrepancies where `_sum < _avg * divider`.

When `dequeue_load_avg()` subtracts independently, the `sub_positive()` on `load_sum` can clamp to zero even though the subtracted entity's contribution (`se_weight(se) * se->avg.load_sum`) might be larger in the `_sum` domain than `se->avg.load_avg` is in the `_avg` domain, due to the divider drift. This creates the situation where `cfs_rq->avg.load_sum == 0` but `cfs_rq->avg.load_avg > 0`, violating the invariant.

The problem is exacerbated by the fact that `se_weight(se)` is involved in the `load_sum` subtraction. If the entity's weight has been modified (via nice value change or cgroup weight change) between the time `load_sum` was last synced and the dequeue, the mismatch can be amplified. The `reweight_entity()` function also calls `dequeue_load_avg()` as part of the reweight operation, providing another path where this inconsistency can manifest.

## Consequence

The immediate observable consequence is a `SCHED_WARN_ON` triggering in `cfs_rq_is_decayed()`, which was added by commit `9e077b52d86a`. This produces a kernel warning (stack trace in dmesg/console) every time the condition is detected. The warning was reported by Sachin Sant during LTP stress testing and scheduler workloads. The patch author Vincent Guittot also independently reproduced the WARN on his own system.

Beyond the warning, the inconsistency between `load_sum` and `load_avg` can lead to incorrect load tracking decisions. The `load_avg` value is used extensively in CFS load balancing (`task_h_load()`, `update_cfs_share()`, `update_tg_load_avg()`) to determine how much load is present on a run queue. If `load_sum` is zero but `load_avg` is positive, functions that rely on the relationship between these values (such as `cfs_rq_is_decayed()` which determines whether blocked load updates should continue) may make incorrect decisions. In particular, `cfs_rq_is_decayed()` returns `true` only when all `_sum` values are zero — if `load_sum` hits zero prematurely while `load_avg` is still positive, the CFS run queue might be incorrectly considered "decayed" and stop receiving blocked load updates, potentially causing stale load tracking values for task group hierarchies.

In the worst case, this could lead to load balancing making incorrect migration decisions based on stale or inconsistent load values, resulting in suboptimal task placement and throughput degradation. However, the primary symptom reported was the SCHED_WARN_ON firing repeatedly under load.

## Fix Summary

The fix modifies `dequeue_load_avg()` to recompute `load_sum` from `load_avg` after the subtraction, instead of independently subtracting `load_sum`:

```c
static inline void
dequeue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    u32 divider = get_pelt_divider(&se->avg);
    sub_positive(&cfs_rq->avg.load_avg, se->avg.load_avg);
    cfs_rq->avg.load_sum = cfs_rq->avg.load_avg * divider;
}
```

After subtracting the entity's `load_avg` from the CFS run queue's `load_avg` (with clamping to zero via `sub_positive()`), the fix recalculates `load_sum` as `load_avg * divider`. This ensures that `load_sum` and `load_avg` remain perfectly in sync after the dequeue operation. If `load_avg` is clamped to zero, then `load_sum` will also be zero; if `load_avg` remains positive, `load_sum` will be the correctly scaled value.

This approach mirrors the fix applied in commit `1c35b07e6d39` for the "removed" entity path in `update_cfs_rq_load_avg()`, where the same pattern of replacing independent `sub_positive()` calls with `_sum = _avg * divider` recalculations was used for `load_sum`, `util_sum`, and `runnable_sum`. The divider used is `get_pelt_divider(&se->avg)`, which returns `PELT_MIN_DIVIDER + se->avg.period_contrib`. This is correct because the se's period_contrib reflects the last PELT update point, and the relationship between `_sum` and `_avg` is defined through this divider.

## Triggering Conditions

The bug requires:

- **CONFIG_SMP=y**: The `dequeue_load_avg()` function only has a non-empty body under `CONFIG_SMP`. On `!CONFIG_SMP` kernels, it is a no-op.
- **CONFIG_FAIR_GROUP_SCHED**: While the bug can theoretically trigger without cgroup support (any CFS entity dequeue), the SCHED_WARN_ON check that detects it is in `cfs_rq_is_decayed()`, which is only called in the `__update_blocked_fair()` path under `CONFIG_FAIR_GROUP_SCHED`. So detecting the bug via the WARN requires fair group scheduling.
- **Multiple CFS tasks**: At least one task must be enqueued and then dequeued from a CFS run queue, with the dequeue happening at a point where `period_contrib` has drifted since the last time `_sum` and `_avg` were perfectly synchronized.
- **PELT divider drift**: The bug requires that `period_contrib` has been incremented (by PELT decay processing) between the last time `load_sum` was set and the dequeue. This happens naturally during normal operation as time passes and `__update_load_avg_se()` / `__update_load_avg_cfs_rq()` are called.
- **Sufficient load variation**: Tasks must have accumulated enough PELT load that the difference in rounding between the `_avg` and `_sum` subtractions matters. Stress tests (like LTP scheduler tests) that create and destroy many tasks with varying weights are effective at triggering this.

The bug was reported as being triggered during LTP stress testing and general scheduler workloads. It is non-deterministic — it depends on the exact timing of PELT updates relative to entity dequeues — but is reliably reproducible under sustained multi-task workloads over time (Sachin Sant ran LTP tests for several hours).

The `reweight_entity()` path (triggered by nice value changes or cgroup weight changes) is another trigger, as it calls `dequeue_load_avg()` followed by `enqueue_load_avg()` with potentially changed weights. Weight changes amplify the mismatch because the `load_sum` subtraction used the old weight-scaled value while `load_avg` subtraction was weight-independent.

## Reproduce Strategy (kSTEP)

### Why this bug cannot be reproduced with kSTEP

1. **KERNEL VERSION TOO OLD**: The fix commit `ceb6ba45dc80` is tagged at `v5.13-rc6` and was merged into `v5.14-rc1`. kSTEP supports Linux v5.15 and newer only. The buggy kernel (the parent of this commit, `ceb6ba45dc80~1`) is in the v5.13-rc6 timeframe, well before v5.15. The kSTEP kernel module framework, build system, and internal APIs are designed for v5.15+ kernels, and building/running on a v5.13 kernel would require fundamental compatibility work.

2. **Both the bug-detection mechanism and the fix are pre-v5.15**: The `SCHED_WARN_ON` check in `cfs_rq_is_decayed()` (commit `9e077b52d86a`) and the fix itself (commit `ceb6ba45dc80`) were both merged during the v5.14-rc1 development cycle. By the time v5.14 was released (and certainly by v5.15), both the detection mechanism and the fix were already present. This means there is no v5.15+ kernel that has the bug — any v5.15+ kernel already includes this fix.

### What would need to be added to kSTEP

If kSTEP were to support pre-v5.15 kernels (specifically v5.13 era), the following would be needed:

- **Kernel version compatibility layer**: kSTEP's internal.h, driver.h, and build system would need adaptation for v5.13 kernel API changes (different header layouts, missing or renamed functions, different struct layouts).
- **PELT observation helpers**: A helper to read `cfs_rq->avg.load_sum`, `cfs_rq->avg.load_avg`, and `se->avg.period_contrib` would allow checking the invariant. kSTEP already provides `cpu_rq(cpu)` and access to `cfs_rq` internals, but the exact struct fields may differ across kernel versions.

### Alternative reproduction methods outside kSTEP

The bug can be reproduced outside kSTEP by:

1. **Building a v5.13 kernel with the SCHED_WARN_ON patch (9e077b52d86a) but without the fix (ceb6ba45dc80)**: Apply commit `9e077b52d86a` and `1c35b07e6d39` but not `ceb6ba45dc80`, then boot the kernel.
2. **Running LTP scheduler stress tests**: The `runltp -f sched` test suite, or sustained workloads that create/destroy many CFS tasks with varying nice values and cgroup memberships, will trigger the WARN within hours.
3. **Monitoring dmesg for SCHED_WARN_ON**: The warning will appear in `dmesg` or on the console as a kernel warning with a stack trace pointing to `cfs_rq_is_decayed()`.
4. **Nice value changes under load**: Running `renice` on tasks while they are actively scheduled can trigger the `reweight_entity() → dequeue_load_avg()` path, amplifying the desync and triggering the WARN more quickly.
