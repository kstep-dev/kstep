# PELT avg/sum Consistency
**Source bug:** `7c7ad626d9a0ff0a36c1e2a3cfbbc6a13828d5eb`

**Property:** For any cfs_rq, if a PELT `*_avg` field is nonzero then the corresponding `*_sum` field must also be nonzero, and vice versa.

**Variables:**
- `cfs_rq->avg.load_avg` — averaged load contribution of this cfs_rq. Read in-place from the `cfs_rq` struct.
- `cfs_rq->avg.load_sum` — accumulated load sum from which `load_avg` is derived. Read in-place from the `cfs_rq` struct.
- (Same applies to `util_avg`/`util_sum` and `runnable_avg`/`runnable_sum` pairs.)

**Check(s):**

Check 1: Performed after `update_tg_cfs_load()` returns (or more broadly, at any point after PELT updates complete for a cfs_rq, e.g., end of `update_blocked_averages()`, `update_load_avg()`, `dequeue_load_avg()`, `attach_entity_load_avg()`). No preconditions — applies to all cfs_rq instances.
```c
// For each cfs_rq that was just updated:
if (cfs_rq->avg.load_avg && !cfs_rq->avg.load_sum)
    BUG("load_avg=%lu nonzero but load_sum==0", cfs_rq->avg.load_avg);
if (!cfs_rq->avg.load_avg && cfs_rq->avg.load_sum)
    BUG("load_avg==0 but load_sum=%llu nonzero", cfs_rq->avg.load_sum);

// Same for util:
if (cfs_rq->avg.util_avg && !cfs_rq->avg.util_sum)
    BUG("util_avg=%lu nonzero but util_sum==0", cfs_rq->avg.util_avg);

// Same for runnable:
if (cfs_rq->avg.runnable_avg && !cfs_rq->avg.runnable_sum)
    BUG("runnable_avg=%lu nonzero but runnable_sum==0", cfs_rq->avg.runnable_avg);
```

**Example violation:** In the buggy `update_tg_cfs_load()`, `add_positive()` independently clamps `load_sum` to zero (because the weighted delta overshoots) while `load_avg` remains positive (because its smaller delta does not overshoot). This causes `load_sum == 0 && load_avg > 0`.

**Other bugs caught:**
- `ceb6ba45dc8074d2a1ec1117463dc94a20d4203d` — `dequeue_load_avg()` independently subtracts `load_sum` and `load_avg` with `sub_positive()`, same desync pattern.
- `40f5aa4c5eaebfeaca4566217cb9c468e28ed682` — `attach_entity_load_avg()` truncates `load_sum` to zero via integer division while `load_avg` remains 1, same invariant violation.
