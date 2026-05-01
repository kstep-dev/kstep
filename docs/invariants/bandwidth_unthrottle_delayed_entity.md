# Delayed Group Entity Must Not Have Runnable Children
**Source bug:** `9b5ce1a37e904fac32d560668134965f4e937f6c`

**Property:** If a group scheduling entity has `sched_delayed == true`, then its group `cfs_rq` must have `h_nr_running == 0`.

**Variables:**
- `se->sched_delayed` — whether the entity is in delayed-dequeue state. Read directly from the `sched_entity` struct at check time.
- `group_cfs_rq(se)->h_nr_running` — number of hierarchically runnable tasks under this group entity. Read directly from the `cfs_rq` struct at check time.
- `se->my_q` — pointer to the group's own `cfs_rq` (non-NULL for group entities). Used to identify group entities and access their `h_nr_running`.

**Check(s):**

Check 1: Performed at the end of `unthrottle_cfs_rq()`, after the hierarchy walk completes. For each ancestor entity that was touched during the walk:
```c
// For every group sched_entity se in the hierarchy:
if (se->my_q && se->sched_delayed) {
    SCHED_WARN_ON(se->my_q->h_nr_running > 0);
}
```

Check 2: Performed at `enqueue_entity()` exit. After an entity is enqueued into a `cfs_rq`, check the parent group entity that owns this `cfs_rq`:
```c
// After enqueue_entity(cfs_rq, se, flags):
struct sched_entity *parent = cfs_rq->tg->se[cpu_of(rq_of(cfs_rq))];
if (parent && parent->sched_delayed) {
    SCHED_WARN_ON(cfs_rq->h_nr_running > 0);
}
```

Check 3: Performed at `scheduler_tick()` or `update_curr()` as a periodic consistency check. Walk group entities on each CPU's rq:
```c
// For each group entity se on this CPU:
if (se->my_q && se->sched_delayed && se->my_q->h_nr_running > 0) {
    // Invariant violated: delayed entity has runnable children
    WARN_ONCE(1, "sched_delayed group entity has h_nr_running=%u",
              se->my_q->h_nr_running);
}
```

**Example violation:** During `unthrottle_cfs_rq()`, child entity B is re-enqueued into A's `cfs_rq` (incrementing `h_nr_running`), but A's group entity in the root `cfs_rq` remains `sched_delayed = true` because the old code broke out of the walk without clearing the delayed state. This leaves A with `sched_delayed == true` and `group_cfs_rq(A)->h_nr_running > 0`.

**Other bugs caught:** Potentially `bandwidth_throttle_block_delayed` and any future bug where an enqueue/unthrottle path adds runnable tasks under a group entity without first resolving its delayed-dequeue state.
