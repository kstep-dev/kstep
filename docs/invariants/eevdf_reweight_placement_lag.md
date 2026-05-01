# Weighted Lag Preservation Across Reweight
**Source bug:** `6d71a9c6160479899ee744d2c6d6602a191deb1f`

**Property:** When `reweight_entity()` changes an on_rq entity's weight, the entity's absolute (weighted) lag `w_i * (V - v_i)` must be preserved.

This follows from EEVDF Corollary #2 (proved in the removed code comments): reweight does NOT affect V, and by construction the absolute lag `lag_i = w_i * (V - v_i)` must be invariant across weight changes. If the implementation correctly preserves the lag, then `w_old * (V - v_old) = w_new * (V_new - v_new)` and `V = V_new`.

**Variables:**
- `weighted_lag_before` — `se->load.weight * (avg_vruntime(cfs_rq) - se->vruntime)` while entity is on the tree. Recorded at entry to `reweight_entity()`, after `update_curr()` but before dequeue/weight change. Must be saved in a shadow variable since the entity will be dequeued and reweighted.
- `weighted_lag_after` — `se->load.weight * (avg_vruntime(cfs_rq) - se->vruntime)` after entity is back on the tree. Recorded at exit of `reweight_entity()`, after `place_entity()` and `__enqueue_entity()`.
- `avg_vruntime_before` — `avg_vruntime(cfs_rq)` recorded at same point as `weighted_lag_before`. Used for the secondary V-preservation check.
- `avg_vruntime_after` — `avg_vruntime(cfs_rq)` recorded at same point as `weighted_lag_after`.

**Check(s):**

Check 1: Performed at exit of `reweight_entity()`. Only when `se->on_rq` and `cfs_rq->nr_running > 1` (with only one entity, lag is trivially zero).
```c
// At entry to reweight_entity(), after update_curr(cfs_rq):
s64 wlag_before = (s64)se->load.weight * (avg_vruntime(cfs_rq) - se->vruntime);
u64 V_before = avg_vruntime(cfs_rq);

// ... reweight proceeds ...

// At exit of reweight_entity(), after place_entity + __enqueue_entity:
s64 wlag_after = (s64)se->load.weight * (avg_vruntime(cfs_rq) - se->vruntime);

// Weighted lag should be approximately preserved.
// Allow tolerance for integer division truncation, proportional to weight ratio.
s64 tolerance = abs(wlag_before) / 16 + TICK_NSEC;
WARN_ON_ONCE(abs(wlag_after - wlag_before) > tolerance);
```

Check 2 (secondary): avg_vruntime preservation — performed at same exit point.
```c
u64 V_after = avg_vruntime(cfs_rq);
// V should not change across reweight (Corollary #2).
// Small tolerance for update_curr() charging and integer rounding.
s64 V_drift = (s64)(V_after - V_before);
WARN_ON_ONCE(abs(V_drift) > TICK_NSEC);
```

**Example violation:** The buggy `reweight_eevdf()` computes `se->vruntime = V - scaled_vlag` without the lag inflation factor `(W + w_i) / W` that `place_entity()` applies. This systematically under-compensates the lag on each reweight, causing `wlag_after` to be smaller in magnitude than `wlag_before` by a factor of `W / (W + w_i)`. Over repeated reweight cycles, the lag drifts unboundedly.

**Other bugs caught:** Potentially `eevdf_reweight_vruntime_unadjusted` (if vruntime is left unadjusted, weighted lag changes), `eevdf_reweight_stale_avg_vruntime` (if stale V is used, the lag computation is wrong), and other reweight bugs where the vruntime/lag relationship is broken.
