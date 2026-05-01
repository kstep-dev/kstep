# CFS runqueue effective runnable count consistency
**Source bug:** `76f2f783294d7d55c2564e2dfb0a7279ba0bc264`

**Property:** The effective runnable count used in cfs_rq-level PELT updates (`h_nr_running - h_nr_delayed`) must equal the actual number of non-delayed runnable entities in the cfs_rq's hierarchy.

**Variables:**
- `cfs_rq->h_nr_running` — hierarchical count of all enqueued entities (including delayed). Read in-place at check points.
- `cfs_rq->h_nr_delayed` — hierarchical count of entities with `sched_delayed == 1`. Read in-place at check points.
- `actual_delayed` — count of entities with `sched_delayed == 1` visible in the cfs_rq hierarchy. Computed by walking sched_entities on the cfs_rq at check time.

**Check(s):**

Check 1: Performed at `__update_load_avg_cfs_rq()`. Always.
```c
// Bounds check: the delayed count must never exceed total running count
// and must never be negative (unsigned underflow).
SCHED_WARN_ON(cfs_rq->h_nr_delayed > cfs_rq->h_nr_running);
```

Check 2: Performed at `__update_load_avg_cfs_rq()` or `scheduler_tick()` (debug/audit mode). When `cfs_rq` is not throttled.
```c
// Walk the cfs_rq's rb-tree and count delayed entities
unsigned int actual_delayed = 0;
struct rb_node *node;
for (node = rb_first_cached(&cfs_rq->tasks_timeline); node; node = rb_next(node)) {
    struct sched_entity *se = rb_entry(node, struct sched_entity, run_node);
    if (se->sched_delayed)
        actual_delayed++;
}
// For a root cfs_rq (no group scheduling), h_nr_delayed must match
// For group cfs_rq, h_nr_delayed includes children's delayed counts hierarchically
// At minimum for the leaf level:
if (!cfs_rq->tg || cfs_rq->tg == &root_task_group) {
    SCHED_WARN_ON(cfs_rq->h_nr_delayed != actual_delayed);
}
```

Check 3: Performed at `se_update_runnable()`. When `!entity_is_task(se)`.
```c
// Group entity runnable_weight must reflect only truly runnable entities
struct cfs_rq *my_q = se->my_q;
SCHED_WARN_ON(se->runnable_weight != my_q->h_nr_running - my_q->h_nr_delayed);
```

**Example violation:** On the buggy kernel, `h_nr_delayed` does not exist (effectively 0), so `h_nr_running` is passed directly as the runnable parameter to `___update_load_sum()`. When 3 of 4 tasks are in delayed-dequeue state, PELT computes runnable_avg as if 4 tasks are runnable instead of 1, inflating the signal and biasing load balancing. Check 2 would detect that the actual delayed count (3) disagrees with the tracked count (0). Check 3 would detect group entity `runnable_weight` includes delayed entities.

**Other bugs caught:** None known, but this invariant would catch any future bug where `h_nr_delayed` accounting drifts from actual entity state (e.g., missed increment/decrement in enqueue/dequeue/throttle/unthrottle paths).
