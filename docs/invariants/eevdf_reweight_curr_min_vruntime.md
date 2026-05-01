# min_vruntime Consistency After Vruntime Modification
**Source bug:** `5068d84054b766efe7c6202fc71b2350d1c326f1`

**Property:** At the end of any operation that modifies an on-rq CFS entity's vruntime, `cfs_rq->min_vruntime` must be consistent with the actual vruntimes of on-rq entities — i.e., it must equal the value that a fresh call to `update_min_vruntime()` would produce.

**Variables:**
- `actual_min_vruntime` — the `cfs_rq->min_vruntime` as stored in the cfs_rq. Read directly from `cfs_rq->min_vruntime` at the check point.
- `expected_min_vruntime` — the value `update_min_vruntime()` would compute freshly. Computed from `cfs_rq->curr->vruntime` (if curr is on-rq) and `__pick_root_entity(cfs_rq)->min_vruntime` (if rb-tree is non-empty), taking the minimum of the two, then clamping to be >= the previous `min_vruntime`. This must be computed inline at the check point.

**Check(s):**

Check 1: Performed at the end of `reweight_entity()`, when `se->on_rq` is true. This catches any reweight path (current or non-current entity) that forgets to update min_vruntime after adjusting vruntime via `reweight_eevdf()`.
```c
// At the end of reweight_entity(), after all updates, when se->on_rq:
if (se->on_rq) {
    struct sched_entity *curr = cfs_rq->curr;
    struct sched_entity *left = __pick_root_entity(cfs_rq);
    u64 vruntime = cfs_rq->min_vruntime;

    if (curr && curr->on_rq)
        vruntime = curr->vruntime;
    else
        curr = NULL;

    if (left) {
        if (!curr)
            vruntime = left->min_vruntime;
        else
            vruntime = min_vruntime(vruntime, left->min_vruntime);
    }

    u64 expected = max_vruntime(cfs_rq->min_vruntime, vruntime);
    WARN_ON_ONCE(cfs_rq->min_vruntime != expected);
}
```

Check 2: Performed at the end of `scheduler_tick()` (or equivalently in an `on_tick_end` callback), for each cfs_rq on the ticked CPU. This is a broader check that catches any code path within a tick that modifies vruntime but forgets to call `update_min_vruntime()`.
```c
// For each cfs_rq on the ticked CPU's rq hierarchy:
struct sched_entity *curr = cfs_rq->curr;
struct sched_entity *left = __pick_root_entity(cfs_rq);
u64 vruntime = cfs_rq->min_vruntime;

if (curr && curr->on_rq)
    vruntime = curr->vruntime;
else
    curr = NULL;

if (left) {
    if (!curr)
        vruntime = left->min_vruntime;
    else
        vruntime = min_vruntime(vruntime, left->min_vruntime);
}

u64 expected = max_vruntime(cfs_rq->min_vruntime, vruntime);
// min_vruntime must not be stale (behind the expected value)
WARN_ON_ONCE(cfs_rq->min_vruntime != expected);
```

**Example violation:** When `reweight_entity()` reweights `cfs_rq->curr` (a group entity whose weight changed), `reweight_eevdf()` adjusts `curr->vruntime`, but the buggy code skips `update_min_vruntime()` for current entities. After the reweight, `cfs_rq->min_vruntime` remains at its old value instead of advancing to reflect curr's new (potentially higher) vruntime, causing `expected != actual`.

**Other bugs caught:** Potentially `eevdf_reweight_stale_avg_vruntime` and `eevdf_reweight_dequeue_avruntime` if they involve stale min_vruntime from missed updates.
