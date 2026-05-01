# h_nr_running Hierarchical Consistency
**Source bug:** `5ab297bab984310267734dfbcc8104566658ebef`

**Property:** For any cfs_rq, `h_nr_running` must equal the number of directly enqueued task entities plus the sum of `h_nr_running` of all enqueued child group entities' owned cfs_rqs.

**Variables:**
- `cfs_rq->h_nr_running` — the cached hierarchical running count. Read in-place from the cfs_rq struct at the check point.
- `computed_h_nr` — the recomputed count by iterating enqueued entities on the cfs_rq. Computed at check time by walking `cfs_rq->tasks` (the rb-tree / entity list). For each enqueued `se`: if it is a task entity, count 1; if it is a group entity, add `group_cfs_rq(se)->h_nr_running`.

**Check(s):**

Check 1: Performed after `enqueue_task_fair()` returns. For each cfs_rq from the task's leaf cfs_rq up to and including the first throttled ancestor (or root if none throttled):
```c
// Walk all enqueued sched_entities on cfs_rq
unsigned int computed = 0;
struct sched_entity *se;
// iterate over all entities on cfs_rq's rb-tree
for_each_enqueued_entity(cfs_rq, se) {
    struct cfs_rq *child = group_cfs_rq(se); // NULL for task entities
    if (child)
        computed += child->h_nr_running;
    else
        computed += 1;
}
WARN_ON_ONCE(cfs_rq->h_nr_running != computed);
```

Check 2: Performed after `dequeue_task_fair()` returns. Same check as above, applied to the same set of ancestor cfs_rqs.
```c
// identical computation as Check 1
unsigned int computed = 0;
struct sched_entity *se;
for_each_enqueued_entity(cfs_rq, se) {
    struct cfs_rq *child = group_cfs_rq(se);
    if (child)
        computed += child->h_nr_running;
    else
        computed += 1;
}
WARN_ON_ONCE(cfs_rq->h_nr_running != computed);
```

**Example violation:** When a task is enqueued in a child cgroup whose parent is throttled, the buggy code skips `h_nr_running++` on the throttled parent's cfs_rq. The consistency check would find that the recomputed sum (reflecting the newly enqueued child) exceeds the stale `h_nr_running` by 1.

**Other bugs caught:** This invariant would also catch any other bug that skips or double-counts `h_nr_running` updates during enqueue/dequeue, task migration, or cgroup moves — potentially including `bandwidth_throttle_runnable_avg` and similar h_nr_running accounting errors in the bandwidth family.
