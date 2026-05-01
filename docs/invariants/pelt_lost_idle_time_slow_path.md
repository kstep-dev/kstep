# PELT clock_pelt must be synced when rq goes idle at max utilization
**Source bug:** `17e3e88ed0b6318fde0d1c14df1a804711cab1b5`

**Property:** When a runqueue transitions to idle and its aggregate PELT util_sum is at or above the maximum threshold, `clock_pelt` must equal `rq_clock_task(rq)` (with the prior gap absorbed into `lost_idle_time`).

**Variables:**
- `clock_pelt_before` — `rq->clock_pelt` snapshot taken just before the rq transitions to idle. Recorded at the point where `pick_next_task()` selects the idle task. Read directly from `rq->clock_pelt`.
- `rq_clock_task_before` — `rq_clock_task(rq)` snapshot at the same point. Read via `rq_clock_task(rq)`.
- `lost_idle_time_before` — `rq->lost_idle_time` before the idle transition. Read directly from `rq->lost_idle_time`.
- `lost_idle_time_after` — `rq->lost_idle_time` after the idle transition completes. Read directly from `rq->lost_idle_time`.
- `total_util_sum` — `rq->cfs.avg.util_sum + rq->avg_rt.util_sum + rq->avg_dl.util_sum`. Computed at the idle transition point.
- `pelt_divider` — `((LOAD_AVG_MAX - 1024) << SCHED_CAPACITY_SHIFT) - LOAD_AVG_MAX`. This is the threshold above which lost idle time must be tracked.

**Check(s):**

Check 1: Performed when the idle task is selected to run (i.e., `pick_next_task()` returns NULL for all classes and the idle task is about to be scheduled). Precondition: `rq->nr_running == 0` (rq is going idle).
```c
// At the point the idle task begins executing on an rq that was previously
// running a non-idle task, check that clock_pelt has been properly synced.
u64 total_util = rq->cfs.avg.util_sum + rq->avg_rt.util_sum + rq->avg_dl.util_sum;
u64 divider = ((LOAD_AVG_MAX - 1024) << SCHED_CAPACITY_SHIFT) - LOAD_AVG_MAX;

if (total_util >= divider) {
    // When util is at max, clock_pelt should have been synced to rq_clock_task
    // and the gap absorbed into lost_idle_time.
    // After update_idle_rq_clock_pelt(), clock_pelt == rq_clock_task(rq).
    WARN_ON_ONCE(rq->clock_pelt != rq_clock_task(rq));
}
```

Check 2: Performed after the idle transition. Precondition: total_util_sum was >= pelt_divider at transition time and the rq was not already idle.
```c
// Alternatively, check that lost_idle_time increased by the clock gap.
// Before going idle, record: gap = rq_clock_task(rq) - rq->clock_pelt
// After going idle, verify:
//   rq->lost_idle_time == lost_idle_time_before + gap
u64 gap = rq_clock_task_before - clock_pelt_before;
if (total_util_sum >= pelt_divider && gap > 0) {
    WARN_ON_ONCE(lost_idle_time_after != lost_idle_time_before + gap);
}
```

**Example violation:** When an RT task is the last task on a CPU and it blocks, the slow path in `pick_next_task_fair()` returns NULL without calling `update_idle_rq_clock_pelt()`. The rq goes idle with `clock_pelt` still lagging behind `rq_clock_task` and `lost_idle_time` unchanged, violating the invariant.

**Other bugs caught:** Any future code path that allows an rq to transition to idle without calling `update_idle_rq_clock_pelt()` when utilization is maxed out — e.g., if a new scheduling class is added with its own pick path that bypasses the PELT sync.
