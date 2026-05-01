# PELT _sum >= _avg * PELT_MIN_DIVIDER Consistency
**Source bug:** `98b0d890220d45418cfbc5157b3382e6da5a12ab`

**Property:** For any PELT signal (util, load, runnable) on any sched_avg, `_sum` must always be at least `_avg * PELT_MIN_DIVIDER` (where `PELT_MIN_DIVIDER = LOAD_AVG_MAX - 1024`).

**Variables:**
- `sa->util_sum` — accumulated utilization sum. Read directly from `struct sched_avg` in-place.
- `sa->util_avg` — utilization average. Read directly from `struct sched_avg` in-place.
- `sa->load_sum` — accumulated load sum. Read directly from `struct sched_avg` in-place.
- `sa->load_avg` — load average. Read directly from `struct sched_avg` in-place.
- `sa->runnable_sum` — accumulated runnable sum. Read directly from `struct sched_avg` in-place.
- `sa->runnable_avg` — runnable average. Read directly from `struct sched_avg` in-place.
- `PELT_MIN_DIVIDER` — constant `LOAD_AVG_MAX - 1024` (46717). The minimum possible value of `get_pelt_divider()` (when `period_contrib == 0`).

**Check(s):**

Check 1: Performed at `update_cfs_rq_load_avg()`, after the removed-contribution processing block completes (i.e., after the `if (cfs_rq->removed.nr)` block). No preconditions beyond `sa->util_avg > 0`.
```c
struct sched_avg *sa = &cfs_rq->avg;

// util signal
SCHED_WARN_ON(sa->util_avg && sa->util_sum < (u32)sa->util_avg * PELT_MIN_DIVIDER);

// runnable signal
SCHED_WARN_ON(sa->runnable_avg && sa->runnable_sum < (u32)sa->runnable_avg * PELT_MIN_DIVIDER);
```

Check 2: Performed at `update_tg_cfs_util()` / `update_tg_cfs_runnable()`, after propagating group entity changes to the parent cfs_rq. Same check on the sched_entity's avg.
```c
struct sched_avg *sa = &se->avg;
SCHED_WARN_ON(sa->util_avg && sa->util_sum < (u32)sa->util_avg * PELT_MIN_DIVIDER);
```

Check 3: Performed at `__update_load_avg_se()` / `__update_load_avg_cfs_rq()` return, as a post-condition after any PELT update.
```c
struct sched_avg *sa = &cfs_rq->avg; // or &se->avg
SCHED_WARN_ON(sa->util_avg && sa->util_sum < (u32)sa->util_avg * PELT_MIN_DIVIDER);
```

**Example violation:** When many tasks with rounding errors (+1 in se->util_sum vs cfs_rq->util_sum) are detached between two PELT periodic updates (~1ms), the cumulative rounding drives cfs_rq->util_sum to zero while util_avg remains positive. The fix's `max_t(u32, sa->util_sum, sa->util_avg * PELT_MIN_DIVIDER)` explicitly enforces this invariant.

**Other bugs caught:** Potentially `pelt_dequeue_load_sum_desync`, `pelt_propagate_load_sum_desync`, and `pelt_attach_load_sum_zero` if those bugs cause _sum to drop below the _avg-consistent lower bound.
