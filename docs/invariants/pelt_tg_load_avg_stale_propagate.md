# Decayed cfs_rq Must Have Zero tg_load_avg_contrib
**Source bug:** `02da26ad5ed6ea8680e5d01f20661439611ed776`

**Property:** When a cfs_rq is fully decayed (`cfs_rq_is_decayed()` returns true), its `tg_load_avg_contrib` must be zero.

**Variables:**
- `is_decayed` — whether the cfs_rq has fully decayed (all PELT sums zero). Recorded at `list_del_leaf_cfs_rq()` call site in `__update_blocked_fair()`. Obtained from `cfs_rq_is_decayed(cfs_rq)`.
- `tg_load_avg_contrib` — the cfs_rq's recorded contribution to `tg->load_avg`. Read in-place from `cfs_rq->tg_load_avg_contrib` at the same point.

**Check(s):**

Check 1: Performed at `list_del_leaf_cfs_rq()` in `__update_blocked_fair()`. Precondition: `cfs_rq_is_decayed(cfs_rq)` is true and `cfs_rq->tg != &root_task_group`.
```c
// When a cfs_rq is about to be removed from the leaf list due to full decay,
// its contribution to tg->load_avg must already be zeroed out.
if (cfs_rq_is_decayed(cfs_rq) && cfs_rq->tg != &root_task_group) {
    WARN_ON_ONCE(cfs_rq->tg_load_avg_contrib != 0);
}
```

Check 2: Performed at `on_sched_softirq_end` or after any `update_blocked_averages()` call. Precondition: `cfs_rq->avg.load_avg == 0` and `cfs_rq->tg != &root_task_group`.
```c
// More generally: if a cfs_rq's load_avg is zero, tg_load_avg_contrib
// must also be zero (they must stay in sync).
if (cfs_rq->avg.load_avg == 0 && cfs_rq->tg != &root_task_group) {
    WARN_ON_ONCE(cfs_rq->tg_load_avg_contrib != 0);
}
```

**Example violation:** The child cfs_rq's decay propagates to the parent cfs_rq without `UPDATE_TG`, so the parent's `tg_load_avg_contrib` remains stale at a non-zero value even after `avg.load_avg` reaches zero. When `cfs_rq_is_decayed()` triggers removal from the leaf list, `tg_load_avg_contrib` is still non-zero, permanently inflating `tg->load_avg`.

**Other bugs caught:** None known, but this invariant would catch any future bug where a code path modifies a cfs_rq's PELT load values without calling `update_tg_load_avg()` to reconcile the contribution.
