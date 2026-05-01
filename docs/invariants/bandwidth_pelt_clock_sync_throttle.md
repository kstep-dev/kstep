# PELT Clock Frozen Consistency for Throttled Empty cfs_rqs
**Source bug:** `0e4a169d1a2b630c607416d9e3739d80e176ed67`

**Property:** If a cfs_rq is in a throttled hierarchy (throttle_count > 0) and has no enqueued entities (nr_running == 0), then its PELT clock must be frozen (pelt_clock_throttled == 1).

**Variables:**
- `cfs_rq->throttle_count` — number of throttled ancestors (including self). Read in-place from the cfs_rq struct at check time.
- `cfs_rq->pelt_clock_throttled` — whether the PELT clock is frozen for this cfs_rq. Read in-place from the cfs_rq struct at check time.
- `cfs_rq->nr_running` — number of runnable entities on this cfs_rq. Read in-place from the cfs_rq struct at check time.

**Check(s):**

Check 1: Performed at exit of `sync_throttle()`. After syncing throttle state for a newly onlined task group's cfs_rq.
```c
// At the end of sync_throttle(), after throttle_count is synced:
if (cfs_rq->throttle_count && !cfs_rq->nr_running)
    WARN_ON_ONCE(!cfs_rq->pelt_clock_throttled);
```

Check 2: Performed at exit of `propagate_entity_cfs_rq()`. For each cfs_rq visited during propagation.
```c
// After the for_each_sched_entity loop in propagate_entity_cfs_rq():
for_each_sched_entity(se) {
    cfs_rq = cfs_rq_of(se);
    if (cfs_rq->throttle_count && !cfs_rq->nr_running)
        WARN_ON_ONCE(!cfs_rq->pelt_clock_throttled);
}
```

Check 3: Performed at `enqueue_entity()` / `dequeue_entity()` boundaries. When nr_running transitions to/from 0 on a throttled hierarchy.
```c
// After dequeue_entity() decrements nr_running:
if (cfs_rq->throttle_count && !cfs_rq->nr_running)
    WARN_ON_ONCE(!cfs_rq->pelt_clock_throttled);
```

**Example violation:** When a new cgroup is created under a throttled parent, `sync_throttle()` sets `throttle_count` from the parent but fails to set `pelt_clock_throttled = 1`. The new empty cfs_rq (nr_running == 0) has `throttle_count > 0` but `pelt_clock_throttled == 0`, violating the invariant. This causes `propagate_entity_cfs_rq()` to incorrectly add the cfs_rq to the leaf list, corrupting `rq->tmp_alone_branch`.

**Other bugs caught:** Potentially `bandwidth_leaf_list_throttle_fixup`, `bandwidth_nosmp_leaf_list_assert`, and other bugs where leaf cfs_rq list corruption stems from PELT clock state being inconsistent with the throttle hierarchy.
