# nr_running Consistency with Per-Class Task Counts
**Source bug:** `5c66d1b9b30f737fcef85a0b75bfe0590e16b62a`

**Property:** At any call to `sched_update_tick_dependency()`, `rq->nr_running` must equal the sum of per-class task counts (`cfs.h_nr_queued + rt.rt_nr_running + dl.dl_nr_running`).

**Variables:**
- `rq->nr_running` — total runnable tasks on the runqueue. Read in-place at `sched_update_tick_dependency()`.
- `rq->cfs.h_nr_queued` — hierarchical count of CFS tasks. Read in-place.
- `rq->rt.rt_nr_running` — count of RT tasks. Read in-place.
- `rq->dl.dl_nr_running` — count of DL tasks. Read in-place.

**Check(s):**

Check 1: Performed at `sched_update_tick_dependency()` (called from `add_nr_running()` and `sub_nr_running()`). Always checked.
```c
unsigned int class_sum = rq->cfs.h_nr_queued + rq->rt.rt_nr_running + rq->dl.dl_nr_running;
if (rq->nr_running != class_sum) {
    // Invariant violation: per-class counts are stale relative to rq->nr_running
    WARN_ONCE(1, "nr_running=%u != class_sum=%u (cfs=%u rt=%u dl=%u)",
              rq->nr_running, class_sum,
              rq->cfs.h_nr_queued, rq->rt.rt_nr_running, rq->dl.dl_nr_running);
}
```

**Example violation:** In the buggy RT dequeue path, `sub_nr_running()` decrements `rq->nr_running` and calls `sched_update_tick_dependency()` before `dec_rt_tasks()` decrements `rt_nr_running`. At this point `rq->nr_running < cfs.h_nr_queued + rt.rt_nr_running + dl.dl_nr_running`, causing `sched_can_stop_tick()` to see stale `rt_nr_running > 0` and incorrectly allow the tick to stop.

**Other bugs caught:** Any future ordering bug in any scheduler class where `add_nr_running()`/`sub_nr_running()` is called before the class-specific counter is updated.
