# PELT Clock Domain Consistency for per-rq sched_avg
**Source bug:** `84d265281d6cea65353fc24146280e0d86ac50cb`

**Property:** Each per-rq `sched_avg` instance's `last_update_time` must remain in its designated clock domain; specifically, the gap between the designated clock and `last_update_time` must be bounded by a small number of tick periods.

**Variables:**
- `last_update_time` — the `last_update_time` field of `rq->avg_hw`. Read in-place from `rq->avg_hw.last_update_time` at the check point.
- `rq_clock_task` — the current value of `rq_clock_task(rq)`, the designated clock source for hw_pressure. Read in-place at the check point.

**Check(s):**

Check 1: Performed after `update_hw_load_avg()` returns, in both `sched_tick()` and `__update_blocked_others()`. Precondition: `rq->avg_hw.last_update_time > 0` (i.e., hw_pressure has been initialized).
```c
// After any call to update_hw_load_avg(), the hw_pressure sched_avg's
// last_update_time should be close to rq_clock_task(rq), since hw_pressure
// uses the task clock domain. A large gap indicates a clock domain mismatch
// (e.g., last_update_time was set using rq_clock_pelt instead).
u64 task_clock = rq_clock_task(rq);
u64 lut = rq->avg_hw.last_update_time;

if (lut > 0) {
    s64 gap = (s64)(task_clock - lut);
    // gap should be non-negative and small (within 2 tick periods).
    // A negative gap means last_update_time is ahead of rq_clock_task,
    // which is impossible if the correct clock is used.
    // A gap >> TICK_NSEC means last_update_time fell behind because it
    // was set in a slower clock domain (rq_clock_pelt at reduced capacity).
    WARN_ON_ONCE(gap < 0 || gap > 2 * TICK_NSEC);
}
```

**Example violation:** On the buggy kernel, `__update_blocked_others()` passes `rq_clock_pelt(rq)` to `update_hw_load_avg()`, setting `last_update_time` in the PELT clock domain. When the CPU runs at reduced capacity, `rq_clock_pelt` lags behind `rq_clock_task`, so `rq_clock_task(rq) - last_update_time` becomes much larger than a tick period, violating the bound.

**Other bugs caught:** `64eaf50731ac0a8c76ce2fedd50ef6652aabc5ff` (PELT throttled clock mismatch — same pattern of subtracting task-domain values from pelt-domain values in `cfs_rq_clock_pelt()`).
