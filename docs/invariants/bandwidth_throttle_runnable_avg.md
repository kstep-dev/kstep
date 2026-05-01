# Group Entity runnable_weight Consistency
**Source bug:** `6212437f0f6043e825e021e4afc5cd63e248a2b4`

**Property:** For any group scheduling entity `se` that is on a runqueue (`se->on_rq == 1`), `se->runnable_weight` must equal `se->my_q->h_nr_running`.

**Variables:**
- `se->runnable_weight` — the cached runnable weight of the group entity, used by PELT to compute `runnable_avg` for the parent `cfs_rq`. Read directly from `struct sched_entity`. Set by `se_update_runnable(se)`.
- `se->my_q->h_nr_running` — the hierarchical count of runnable tasks in the `cfs_rq` owned by this group entity. Read directly from `struct cfs_rq`. Modified by enqueue/dequeue and throttle/unthrottle operations.

**Check(s):**

Check 1: Performed at the end of `throttle_cfs_rq()`, after the `for_each_sched_entity` loop completes. Precondition: `CONFIG_FAIR_GROUP_SCHED` and `CONFIG_CFS_BANDWIDTH` enabled.
```c
// Walk the hierarchy from the throttled cfs_rq's group se upward.
// For each group entity that is still on its runqueue, verify consistency.
struct sched_entity *check_se = cfs_rq->tg->se[cpu_of(rq_of(cfs_rq))];
for_each_sched_entity(check_se) {
    if (!check_se->on_rq)
        break;
    if (!entity_is_task(check_se)) {
        WARN_ON_ONCE(check_se->runnable_weight != check_se->my_q->h_nr_running);
    }
}
```

Check 2: Performed at the end of `unthrottle_cfs_rq()`, after the `for_each_sched_entity` loop completes. Same preconditions.
```c
// Same walk for the unthrottle path.
struct sched_entity *check_se = cfs_rq->tg->se[cpu_of(rq_of(cfs_rq))];
for_each_sched_entity(check_se) {
    if (!check_se->on_rq)
        break;
    if (!entity_is_task(check_se)) {
        WARN_ON_ONCE(check_se->runnable_weight != check_se->my_q->h_nr_running);
    }
}
```

Check 3 (broader): Performed at `update_curr()` or `scheduler_tick()` for any `cfs_rq` with a group entity parent. This catches any code path that modifies `h_nr_running` without updating the parent entity.
```c
// For the current cfs_rq's parent group entity (if any):
if (cfs_rq->tg && cfs_rq->tg->parent) {
    struct sched_entity *se = cfs_rq->tg->se[cpu];
    if (se && se->on_rq && !entity_is_task(se)) {
        WARN_ON_ONCE(se->runnable_weight != se->my_q->h_nr_running);
    }
}
```

**Example violation:** When `throttle_cfs_rq()` walks up a 3-level cgroup hierarchy, the lowest ancestor entity is dequeued (calling `se_update_runnable`), but higher ancestors that remain on the runqueue skip the `else` branch entirely. Their `se->runnable_weight` retains the old `h_nr_running` value (before throttled tasks were subtracted), violating the consistency with the already-decremented `se->my_q->h_nr_running`.

**Other bugs caught:** Potentially `bandwidth_enqueue_dequeue_reorder` (if it involves h_nr_running / runnable_weight desync), and any future bug where a code path modifies `cfs_rq->h_nr_running` without calling `se_update_runnable()` on the parent group entity.
