# Per-RQ PELT last_update_time Synchronized After Class Transition
**Source bug:** `d7d607096ae6d378b4e92d49946d22739c047d4c`

**Property:** After `switched_to_*()` completes for a running task (`rq->curr == p`), the new scheduling class's per-rq PELT structure must have its `last_update_time` synchronized to the current `rq_clock_pelt(rq)`.

**Variables:**
- `last_update_time` — the per-rq PELT timestamp for the scheduling class (e.g., `dl_rq->avg.last_update_time` or `rt_rq->avg.last_update_time`). Read in-place from the rq structure immediately after `switched_to_*()` returns.
- `rq_clock_pelt_now` — current PELT clock for the runqueue. Snapshot of `rq_clock_pelt(rq)` taken at the same point.

**Check(s):**

Check 1: Performed at the return of `switched_to_dl()`. Only when `rq->curr == p` (i.e., the running task switched into DL class).
```c
// After switched_to_dl() returns:
if (rq->curr == p) {
    u64 delta = rq_clock_pelt(rq) - rq->dl.avg.last_update_time;
    // last_update_time should have been synchronized; delta should be
    // near zero (within one tick at most), not seconds stale.
    WARN_ON_ONCE(delta > NSEC_PER_MSEC);
}
```

Check 2: Performed at the return of `switched_to_rt()`. Only when `rq->curr == p`.
```c
// After switched_to_rt() returns:
if (rq->curr == p) {
    u64 delta = rq_clock_pelt(rq) - rq->rt.avg.last_update_time;
    WARN_ON_ONCE(delta > NSEC_PER_MSEC);
}
```

**Example violation:** A CFS task calls `sched_setattr()` to become SCHED_DEADLINE while it is the currently running task. `switched_to_dl()` (buggy version) does nothing when `rq->curr == p`, so `dl_rq->avg.last_update_time` remains stale (potentially seconds or minutes old). The check fires because the delta between `rq_clock_pelt(rq)` and `last_update_time` far exceeds 1ms.

**Other bugs caught:** `2e4b0fa3f03b` (identical bug in `switched_to_rt()` for RT utilization tracking)
