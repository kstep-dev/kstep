# Hierarchical Quota Monotonicity
**Source bug:** `c98c18270be115678f4295b10a5af5dcc9c4efa0`

**Property:** A child task group's `hierarchical_quota` (effective bandwidth constraint) must never be less restrictive than its parent's, i.e., `child_hq ≤ parent_hq` when treating `RUNTIME_INF` as infinity.

**Variables:**
- `child_hq` — `cfs_bandwidth.hierarchical_quota` of the child task group. Read in-place from `tg->cfs_bandwidth.hierarchical_quota` (type `s64`).
- `parent_hq` — `cfs_bandwidth.hierarchical_quota` of the parent task group. Read in-place from `tg->parent->cfs_bandwidth.hierarchical_quota` (type `s64`).

Both are updated by `tg_cfs_schedulable_down()` during `__cfs_schedulable()`, which runs on every `cpu.max` write. Also set at task group creation by `init_cfs_bandwidth()`. No shadow variables needed — read directly from the structs.

**Check(s):**

Check 1: Performed after `tg_cfs_schedulable_down()` stores `cfs_b->hierarchical_quota = quota`. Precondition: `tg->parent != NULL` (skip for root task group).
```c
// After cfs_b->hierarchical_quota = quota; in tg_cfs_schedulable_down()
if (tg->parent) {
    s64 child_hq = tg->cfs_bandwidth.hierarchical_quota;
    s64 parent_hq = tg->parent->cfs_bandwidth.hierarchical_quota;

    // RUNTIME_INF as s64 is -1; a real quota is positive.
    // Child must be at most as permissive as parent:
    //   - If parent_hq == RUNTIME_INF (-1), child can be anything (no ancestor limit)
    //   - If parent_hq is a real (positive) value, child_hq must also be
    //     a real value <= parent_hq (child cannot escape ancestor's constraint)
    if (parent_hq != (s64)RUNTIME_INF) {
        WARN_ON_ONCE(child_hq == (s64)RUNTIME_INF || child_hq > parent_hq);
    }
}
```

Check 2: Performed at `init_cfs_bandwidth()` return. Precondition: `parent != NULL`.
```c
// After init_cfs_bandwidth(cfs_b, parent) completes:
if (parent) {
    WARN_ON_ONCE(cfs_b->hierarchical_quota != parent->hierarchical_quota);
    // A newly created task group with no explicit quota must inherit
    // the parent's hierarchical_quota exactly.
}
```

**Example violation:** On the buggy kernel, when a parent has a real bandwidth limit and a child has `quota == RUNTIME_INF`, `min((s64)RUNTIME_INF, parent_quota)` evaluates to `min(-1, positive_value) == -1`, making the child's `hierarchical_quota` equal to `RUNTIME_INF` despite the parent having a real constraint — violating `child_hq ≤ parent_hq`.

**Other bugs caught:** Any future bug in `tg_cfs_schedulable_down()` or `init_cfs_bandwidth()` that fails to propagate ancestor bandwidth limits downward in the cgroup hierarchy.
