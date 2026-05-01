# Fresh V Before Reweight EEVDF Calculations
**Source bug:** `11b1b8bc2b98e21ddf47e08b56c21502c685b2c3`

**Property:** When `reweight_eevdf()` is called, the current entity's execution must already be committed (via `update_curr()`), so that `avg_vruntime(cfs_rq)` returns an accurate V.

**Variables:**
- `curr_exec_delta` — outstanding uncommitted execution time of `cfs_rq->curr`. Recorded at entry to `reweight_eevdf()`. Computed as `rq_clock_task(rq_of(cfs_rq)) - cfs_rq->curr->exec_start` when `cfs_rq->curr != NULL`.

**Check(s):**

Check 1: Performed at entry to `reweight_eevdf()`. Precondition: `cfs_rq->curr != NULL`.
```c
// At entry to reweight_eevdf(cfs_rq, se, weight):
struct rq *rq = rq_of(cfs_rq);
struct sched_entity *curr = cfs_rq->curr;

if (curr) {
    u64 delta_exec = rq_clock_task(rq) - curr->exec_start;
    // update_curr() sets exec_start = rq_clock_task(rq) after committing.
    // A non-zero delta means update_curr() was not called since the last
    // clock update, so avg_vruntime() will return a stale V.
    WARN_ON_ONCE(delta_exec > 0);
}
```

**Example violation:** When `reweight_entity()` is called for a non-current on-rq entity, the buggy code skips `update_curr()`, leaving `curr->exec_start` behind `rq_clock_task(rq)`. The subsequent `reweight_eevdf()` call then reads a stale V from `avg_vruntime()`, producing incorrect vruntime and deadline adjustments.

**Other bugs caught:** Potentially `eevdf_reweight_dequeue_avruntime` (where V changes between `update_curr` and the point it's consumed in `reweight_eevdf`), and any future bug where `update_curr()` is omitted before a V-dependent calculation in reweight paths.
