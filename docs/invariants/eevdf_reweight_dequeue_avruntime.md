# Reweight Preserves Weighted Lag
**Source bug:** `afae8002b4fd3560c8f5f1567f3c3202c30a70fa`

**Property:** A reweight operation on a CFS scheduling entity must preserve its weighted lag: `w_new * (V_after - v_after) == w_old * (V_before - v_before)`.

**Variables:**
- `w_old` — entity's weight before reweight. Recorded at entry of `reweight_entity()`. Read from `se->load.weight`.
- `v_old` — entity's vruntime before reweight. Recorded at entry of `reweight_entity()`. Read from `se->vruntime`.
- `V_before` — weighted average vruntime of the cfs_rq before reweight. Recorded at entry of `reweight_entity()`, before any dequeue. Obtained via `avg_vruntime(cfs_rq)`.
- `lag_before` — weighted lag before reweight: `(s64)(V_before - v_old) * w_old`. Computed from the above.
- `w_new` — entity's weight after reweight. Recorded at exit of `reweight_entity()`. Read from `se->load.weight`.
- `v_new` — entity's vruntime after reweight. Recorded at exit of `reweight_entity()`. Read from `se->vruntime`.
- `V_after` — weighted average vruntime of the cfs_rq after reweight. Recorded at exit of `reweight_entity()`, after any re-enqueue. Obtained via `avg_vruntime(cfs_rq)`.
- `lag_after` — weighted lag after reweight: `(s64)(V_after - v_new) * w_new`. Computed from the above.

**Check(s):**

Check 1: Performed at exit of `reweight_entity()`. Precondition: `se->on_rq == 1` at entry (entity was on the runqueue during reweight) and `w_old != w_new` (weight actually changed).
```c
// At entry of reweight_entity():
s64 lag_before = (s64)(avg_vruntime(cfs_rq) - se->vruntime) * (s64)se->load.weight;

// At exit of reweight_entity():
s64 lag_after = (s64)(avg_vruntime(cfs_rq) - se->vruntime) * (s64)se->load.weight;

// Allow tolerance for integer division rounding (one weight unit worth of vruntime)
s64 tolerance = max(se->load.weight, old_weight);
WARN_ON_ONCE(abs(lag_after - lag_before) > tolerance);
```

**Example violation:** When `se != cfs_rq->curr`, the buggy code dequeues the entity before capturing V, so `reweight_eevdf()` computes the new vruntime using a V that excludes the entity. This produces an incorrect `v_new`, making `lag_after != lag_before`.

**Other bugs caught:** `eevdf_reweight_stale_avg_vruntime` (uses stale V due to missing `update_curr()` — the companion fix `11b1b8bc2b98`), potentially `eevdf_reweight_vruntime_unadjusted` and `eevdf_reweight_placement_lag` if they also break the lag preservation property during reweight.
