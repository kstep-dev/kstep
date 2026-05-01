# EEVDF Lag Preservation Through Reweight
**Source bug:** `eab03c23c2a162085b13200d7942fc5a00b5ccc8`

**Property:** When an on-rq CFS entity's weight changes, its lag (`w * (V - v)`) must be preserved — i.e., the new vruntime must satisfy `w' * (V - v') = w * (V - v)`.

**Variables:**
- `lag_before` — entity's lag before reweight: `old_weight * (avg_vruntime - se->vruntime)`. Recorded at entry to `reweight_entity()` (or `reweight_eevdf()`), only for on-rq entities. Computed from `se->load.weight`, `se->vruntime`, and `avg_vruntime(cfs_rq)`.
- `lag_after` — entity's lag after reweight: `new_weight * (avg_vruntime - se->vruntime)`. Recorded after `reweight_eevdf()` returns (and before `__enqueue_entity`). Computed from the new weight parameter, `se->vruntime` (now adjusted), and `avg_vruntime(cfs_rq)`. Note: `avg_vruntime(cfs_rq)` is unchanged by reweight (Corollary #2), but the entity has been removed from the avg tracking, so `avg_vruntime` must be sampled before removal or after re-addition for consistency — or equivalently, use the same `V` snapshot for both.

**Check(s):**

Check 1: Performed at `reweight_entity()`, after the on-rq branch completes EEVDF adjustments and before `update_load_set()`. Precondition: `se->on_rq && cfs_rq->nr_running > 1` (at zero-lag or single-entity cfs_rq, the invariant is trivially satisfied).
```c
// Snapshot before reweight (at entry to reweight_entity, after update_curr if curr):
u64 V = avg_vruntime(cfs_rq);  // or use avruntime local in reweight_eevdf
s64 lag_before = (s64)(V - se->vruntime) * (s64)se->load.weight;

// ... reweight_eevdf() executes, adjusting se->vruntime and se->deadline ...

// Snapshot after reweight:
s64 lag_after = (s64)(V - se->vruntime) * (s64)new_weight;

// V is unchanged by reweight (Corollary #2), so same V is used.
// Allow ±new_weight tolerance for integer division rounding.
s64 diff = lag_after - lag_before;
if (diff < 0) diff = -diff;
WARN_ON_ONCE(diff > (s64)new_weight);
```

**Example violation:** On the buggy kernel, `reweight_entity()` changes the entity's weight from `w` to `w'` but leaves `se->vruntime` unchanged. The lag changes from `w*(V-v)` to `w'*(V-v)`, a factor of `w'/w` distortion. For example, reweighting from nice 0 (w=1024) to nice 5 (w=335) at non-zero lag multiplies the effective lag by 335/1024 ≈ 0.33, violating EEVDF fairness.

**Other bugs caught:** Potentially catches `eevdf_reweight_placement_lag`, `eevdf_reweight_stale_avg_vruntime`, `eevdf_reweight_dequeue_avruntime`, and other reweight-related EEVDF bugs where lag is not correctly maintained through weight changes.
