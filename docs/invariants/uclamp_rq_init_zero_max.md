# Uclamp RQ Default Value Consistency
**Source bug:** `d81ae8aac85ca2e307d273f6dc7863a721bf054e`

**Property:** When no tasks contribute to a uclamp clamp_id on a runqueue (all bucket task counts are zero), `rq->uclamp[clamp_id].value` must equal `uclamp_none(clamp_id)`.

**Variables:**
- `bucket_total` — sum of `rq->uclamp[clamp_id].bucket[i].tasks` across all buckets for a given clamp_id. Computed on-demand at check time by iterating `UCLAMP_BUCKETS` entries. Alternatively, can be maintained as a shadow counter incremented/decremented in `uclamp_rq_inc_id()` / `uclamp_rq_dec_id()`.
- `rq_uclamp_value` — `rq->uclamp[clamp_id].value`. Read directly from the rq struct at check time.
- `expected_default` — `uclamp_none(clamp_id)`: 0 for `UCLAMP_MIN`, `SCHED_CAPACITY_SCALE` (1024) for `UCLAMP_MAX`. Known statically.

**Check(s):**

Check 1: Performed at `uclamp_rq_inc_id()` entry (before incrementing), and at `uclamp_rq_dec_id()` exit (after decrementing). Precondition: `CONFIG_UCLAMP_TASK` is enabled.
```c
// After uclamp_rq_dec_id() completes (or before uclamp_rq_inc_id()
// when transitioning from empty):
unsigned int total = 0;
for (int i = 0; i < UCLAMP_BUCKETS; i++)
    total += uc_rq->bucket[i].tasks;

if (total == 0) {
    WARN_ON_ONCE(uc_rq->value != uclamp_none(clamp_id));
}
```

Check 2: Performed at `scheduler_tick()` or any point after boot where runqueue state can be inspected. Precondition: `rq->nr_running == 0` (idle runqueue) and `!(rq->uclamp_flags & UCLAMP_FLAG_IDLE)` (not in the deliberate stale-value window).
```c
// When rq is idle and not in the UCLAMP_FLAG_IDLE stale window:
if (rq->nr_running == 0 && !(rq->uclamp_flags & UCLAMP_FLAG_IDLE)) {
    for_each_clamp_id(clamp_id) {
        WARN_ON_ONCE(rq->uclamp[clamp_id].value != uclamp_none(clamp_id));
    }
}
```

**Example violation:** At boot, `init_uclamp()` uses `memset(&rq->uclamp, 0, ...)` which sets `rq->uclamp[UCLAMP_MAX].value = 0`. No tasks are enqueued (all bucket counts are zero), yet the value is 0 instead of the expected default `uclamp_none(UCLAMP_MAX) = 1024`. Check 1 or Check 2 would fire immediately.

**Other bugs caught:** `315c4f884800c45cb6bd8c90422fad554a8b9588` (uclamp_rq_max_first_enqueue — init omits `UCLAMP_FLAG_IDLE`, causing the rq to not reset uclamp values on first real enqueue, which is a related violation of default-value consistency when bucket counts transition).
