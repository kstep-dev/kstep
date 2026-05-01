# Entity Lag Bounded by Slice
**Source bug:** `1560d1f6eb6b398bddd80c16676776c0325fe5fe`

**Property:** The virtual lag of any CFS scheduling entity (the difference between avg_vruntime and the entity's vruntime) must be bounded by approximately twice the entity's time slice whenever it is used in arithmetic.

**Variables:**
- `vlag` — The entity's virtual lag, computed as `avg_vruntime(cfs_rq) - se->vruntime`. Read in-place at check points from `cfs_rq` and `se` fields.
- `limit` — The maximum allowed magnitude of lag, `calc_delta_fair(max(2 * se->slice, TICK_NSEC), se)`. Computed on-the-fly from `se->slice` and `se->load.weight` at the check point.

**Check(s):**

Check 1: Performed at `reweight_eevdf()` / `reweight_entity()`, after the vlag is computed but before it is used in the scaling multiplication. Precondition: `se->on_rq && avruntime != se->vruntime`.
```c
s64 vlag = (s64)(avruntime - se->vruntime);
s64 limit = calc_delta_fair(max_t(u64, 2 * se->slice, TICK_NSEC), se);

// Invariant: vlag must be within [-limit, limit]
WARN_ON_ONCE(vlag > limit || vlag < -limit);
```

Check 2: Performed at `update_entity_lag()`, before storing to `se->vlag`. Precondition: `se->on_rq`.
```c
s64 lag = avg_vruntime(cfs_rq) - se->vruntime;
s64 limit = calc_delta_fair(max_t(u64, 2 * se->slice, TICK_NSEC), se);

// Invariant: stored vlag must be clamped
WARN_ON_ONCE(se->vlag > limit || se->vlag < -limit);
```

**Example violation:** In the buggy kernel, `reweight_eevdf()` computes `vlag = avruntime - se->vruntime` without clamping. When a group entity accumulates a very large vruntime (e.g., under tick suppression), the raw vlag can reach ~90 billion ns. Multiplying this by `old_weight` (~10^8) overflows s64, corrupting the entity's vruntime and causing `pick_eevdf()` to return NULL.

**Other bugs caught:** Potentially catches any future code path that computes entity lag without clamping (e.g., the `!on_rq` case in `reweight_entity()` mentioned in the commit discussion as a separate but related issue).
