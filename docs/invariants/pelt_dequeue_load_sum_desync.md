# PELT sum-avg zero consistency
**Source bug:** `ceb6ba45dc8074d2a1ec1117463dc94a20d4203d`

**Property:** For any `cfs_rq`, if a PELT `*_sum` field is zero then the corresponding `*_avg` field must also be zero (i.e., `load_sum == 0 → load_avg == 0`, and likewise for `util` and `runnable`).

**Variables:**
- `cfs_rq->avg.load_sum` — weight-scaled geometric sum of load. Read directly from struct field at check point.
- `cfs_rq->avg.load_avg` — weighted average of load. Read directly from struct field at check point.
- `cfs_rq->avg.util_sum` — geometric sum of utilization. Read directly from struct field at check point.
- `cfs_rq->avg.util_avg` — average utilization. Read directly from struct field at check point.
- `cfs_rq->avg.runnable_sum` — geometric sum of runnable. Read directly from struct field at check point.
- `cfs_rq->avg.runnable_avg` — average runnable. Read directly from struct field at check point.

**Check(s):**

Check 1: Performed after `dequeue_load_avg()` returns (or equivalently at the start of `__update_blocked_fair()` / `cfs_rq_is_decayed()`). Applicable when `CONFIG_SMP` and `CONFIG_FAIR_GROUP_SCHED` are enabled.
```c
// For each cfs_rq after an entity dequeue or during blocked load update:
WARN_ON_ONCE(cfs_rq->avg.load_sum == 0 && cfs_rq->avg.load_avg != 0);
WARN_ON_ONCE(cfs_rq->avg.util_sum == 0 && cfs_rq->avg.util_avg != 0);
WARN_ON_ONCE(cfs_rq->avg.runnable_sum == 0 && cfs_rq->avg.runnable_avg != 0);
```

Check 2: Performed after `update_cfs_rq_load_avg()` processes "removed" entity contributions (the subtraction path inside that function). Same predicate as Check 1.
```c
WARN_ON_ONCE(cfs_rq->avg.load_sum == 0 && cfs_rq->avg.load_avg != 0);
WARN_ON_ONCE(cfs_rq->avg.util_sum == 0 && cfs_rq->avg.util_avg != 0);
WARN_ON_ONCE(cfs_rq->avg.runnable_sum == 0 && cfs_rq->avg.runnable_avg != 0);
```

**Example violation:** `dequeue_load_avg()` independently subtracted `load_sum` and `load_avg` using `sub_positive()`. Due to rounding and PELT divider drift, `load_sum` could clamp to zero while `load_avg` remained positive, breaking the invariant.

**Other bugs caught:** `1c35b07e6d39` (same class of desync in `update_cfs_rq_load_avg()` "removed" path for load/util/runnable sums). Likely also catches `pelt_propagate_load_sum_desync`, `pelt_util_sum_sync_loss`, and `pelt_attach_load_sum_zero` if they involve independent `sub_positive()` on sum/avg pairs.
