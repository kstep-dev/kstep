# Pending Removed Load Requires Leaf List Membership
**Source bug:** `0258bdfaff5bd13c4d2383150b7097aecd6b6d82`

**Property:** If a cfs_rq has non-zero pending removed load or util (`cfs_rq->removed`), it must be on the per-CPU `leaf_cfs_rq_list` so that `update_blocked_averages()` can process and decay that load.

**Variables:**
- `removed_load` — `cfs_rq->removed.load_avg`. Read directly from the cfs_rq struct at the check point. Non-zero means load was transferred here by a migrating/detaching entity and is awaiting subtraction.
- `removed_util` — `cfs_rq->removed.util_avg`. Same as above for util.
- `on_list` — `cfs_rq->on_list`. Read directly. Indicates whether the cfs_rq is on the rq's `leaf_cfs_rq_list`, making it visible to `update_blocked_averages()`.

**Check(s):**

Check 1: Performed after `remove_entity_load_avg()` completes (or equivalently, at the start of `update_blocked_averages()` for each rq). For every cfs_rq belonging to a task_group on this rq:
```c
// For each cfs_rq associated with a task_group on this CPU:
if (atomic_long_read(&cfs_rq->removed.load_avg) != 0 ||
    atomic_long_read(&cfs_rq->removed.util_avg) != 0) {
    WARN_ON_ONCE(!cfs_rq->on_list);
}
```

Check 2: Performed at the end of `propagate_entity_cfs_rq()` and `detach_entity_cfs_rq()`, which are the paths that attach/detach load to cfs_rqs for sleeping tasks. Same predicate as above applied to the cfs_rq that was modified.
```c
// After propagate_entity_cfs_rq(se) returns:
struct cfs_rq *cfs_rq = cfs_rq_of(se);
if (atomic_long_read(&cfs_rq->removed.load_avg) != 0 ||
    atomic_long_read(&cfs_rq->removed.util_avg) != 0) {
    WARN_ON_ONCE(!cfs_rq->on_list);
}
```

**Example violation:** A sleeping task is attached to a cgroup's cfs_rq, then migrated to another CPU before ever being enqueued. The migration moves its load into `cfs_rq->removed`, but the cfs_rq was never added to `leaf_cfs_rq_list`. The invariant fires because `removed.load_avg > 0` while `on_list == 0`, meaning this phantom load will never be decayed.

**Other bugs caught:** Potentially `039ae8bcf7a5` (cfs_rq removed from leaf list while it still had pending removed load due to the zero-load optimization).
