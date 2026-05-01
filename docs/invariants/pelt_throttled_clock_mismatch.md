# cfs_rq_clock_pelt Bounded by rq_clock_pelt
**Source bug:** `64eaf50731ac0a8c76ce2fedd50ef6652aabc5ff`

**Property:** For any cfs_rq, `cfs_rq_clock_pelt(cfs_rq)` must never exceed `rq_clock_pelt(rq_of(cfs_rq))`.

**Variables:**
- `cfs_rq_pelt_clock` — the effective PELT clock for a cfs_rq, excluding throttled intervals. Read in-place via `cfs_rq_clock_pelt(cfs_rq)` at each check point.
- `rq_pelt_clock` — the rq-level PELT clock. Read in-place via `rq_clock_pelt(rq_of(cfs_rq))` at each check point.

**Check(s):**

Check 1: Performed at `update_load_avg` (called from `enqueue_entity`, `dequeue_entity`, `entity_tick`, `put_prev_entity`, `set_next_entity`). Whenever a cfs_rq's PELT signals are updated:
```c
// cfs_rq_clock_pelt() is the rq PELT clock minus accumulated throttled time.
// Since throttled time is a non-negative subset of elapsed time,
// the result must be <= the total rq PELT clock.
u64 cfs_clock = cfs_rq_clock_pelt(cfs_rq);
u64 rq_clock  = rq_clock_pelt(rq_of(cfs_rq));
WARN_ON_ONCE(cfs_clock > rq_clock);
```

Check 2: Performed at `tg_unthrottle_up`, when `throttle_count` drops to 0 (cfs_rq becomes unthrottled). The newly computed `cfs_rq_clock_pelt()` must still be bounded:
```c
if (!cfs_rq->throttle_count) {
    u64 cfs_clock = cfs_rq_clock_pelt(cfs_rq);
    u64 rq_clock  = rq_clock_pelt(rq);
    WARN_ON_ONCE(cfs_clock > rq_clock);
}
```

**Example violation:** On the buggy kernel at 50% CPU frequency, `throttled_clock_task_time` accumulates in the task-clock domain (advancing at ~2× the rate of the PELT clock). When subtracted from `rq_clock_pelt()` in `cfs_rq_clock_pelt()`, the over-sized subtraction causes unsigned underflow, wrapping to a value far larger than `rq_clock_pelt(rq)`.

**Other bugs caught:** `84d265281d6cea65353fc24146280e0d86ac50cb` (hw_pressure clock domain mismatch) exhibits the same class of error — mixing clock domains in PELT arithmetic — and could be caught by an analogous bound check on hw_pressure's `last_update_time`.
