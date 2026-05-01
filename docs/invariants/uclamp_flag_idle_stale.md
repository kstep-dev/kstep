# UCLAMP_FLAG_IDLE Consistency with Active Bucket Tasks
**Source bug:** `ca4984a7dd863f3e1c0df775ae3e744bff24c303`

**Property:** If any UCLAMP_MAX bucket on a runqueue has a nonzero task count, then UCLAMP_FLAG_IDLE must not be set on that runqueue.

**Variables:**
- `uclamp_flags` — the runqueue's `rq->uclamp_flags` field. Read in-place at the check point.
- `has_active_uclamp_max` — whether any `rq->uclamp[UCLAMP_MAX].bucket[i].tasks > 0` for any bucket `i`. Computed by iterating `UCLAMP_BUCKETS` at the check point (constant-size array, cheap).

**Check(s):**

Check 1: Performed after `uclamp_rq_inc_id()` returns (covers both enqueue and active-update paths). Only when `clamp_id == UCLAMP_MAX`.
```c
// After any uclamp_rq_inc_id(rq, p, UCLAMP_MAX):
if (rq->uclamp_flags & UCLAMP_FLAG_IDLE) {
    unsigned int i;
    for (i = 0; i < UCLAMP_BUCKETS; i++) {
        if (rq->uclamp[UCLAMP_MAX].bucket[i].tasks > 0) {
            // VIOLATION: flag says idle but bucket has active tasks
            WARN_ONCE(1, "UCLAMP_FLAG_IDLE set with active UCLAMP_MAX bucket %u (tasks=%u)",
                       i, rq->uclamp[UCLAMP_MAX].bucket[i].tasks);
            break;
        }
    }
}
```

Check 2: Performed at `enqueue_task()` after `uclamp_rq_inc()` completes. Unconditional (any enqueue of a uclamp-enabled task).
```c
// After uclamp_rq_inc(rq, p):
if (rq->uclamp_flags & UCLAMP_FLAG_IDLE) {
    WARN_ONCE(1, "UCLAMP_FLAG_IDLE still set after uclamp_rq_inc");
}
```

**Example violation:** `uclamp_update_active()` calls `uclamp_rq_dec_id()` then `uclamp_rq_inc_id()` for the sole active task. The decrement transiently sets `UCLAMP_FLAG_IDLE`, and the increment restores `bucket.tasks > 0` but never clears the flag, leaving the invariant violated.

**Other bugs caught:** Potentially `uclamp_idle_rq_stale_max` and any future bug where a code path modifies uclamp bucket counts without properly synchronizing the idle flag.
