# CFS Runqueue Load Weight Consistency
**Source bug:** `d329605287020c3d1c3b0dadc63d8208e7251382`

**Property:** `cfs_rq->load.weight` must equal the sum of `se->load.weight` for all on-rq sched_entities in that cfs_rq.

**Variables:**
- `cfs_rq->load.weight` — the incrementally maintained aggregate load weight of the cfs runqueue. Read in-place from the `cfs_rq` struct at the check point.
- `computed_sum` — the sum of `se->load.weight` for every `sched_entity` on the cfs_rq (both the rb-tree entities and the current running entity). Computed by walking the rb-tree via `rb_first_cached(&cfs_rq->tasks_timeline)` and iterating with `rb_next()`, plus adding `cfs_rq->curr->load.weight` if `cfs_rq->curr` is non-NULL and on-rq. Computed fresh at each check.

**Check(s):**

Check 1: Performed at `task_tick_fair` (specifically inside `entity_tick()` after `update_curr()` returns). Only when `cfs_rq->nr_running > 0`.
```c
// Walk all entities on the cfs_rq rb-tree and sum their weights.
// Also include curr if it is on-rq (curr is not in the tree while running).
unsigned long computed = 0;
struct rb_node *node;

for (node = rb_first_cached(&cfs_rq->tasks_timeline); node; node = rb_next(node)) {
    struct sched_entity *e = rb_entry(node, struct sched_entity, run_node);
    computed += e->load.weight;
}
if (cfs_rq->curr && cfs_rq->curr->on_rq)
    computed += cfs_rq->curr->load.weight;

WARN_ON_ONCE(cfs_rq->load.weight != computed);
```

Check 2: Performed at exit of `reweight_entity()`. After the entity's weight has been changed and cfs_rq load updated, the same sum-check can be performed to catch the bug at the exact point of the weight change (if `reweight_entity` is actually called). This mainly validates future regressions where `reweight_entity` itself has a bug.
```c
// Same walk as Check 1, performed at end of reweight_entity().
// This catches bugs within reweight_entity's own load accounting.
```

**Example violation:** When a CFS task transitions to SCHED_IDLE, `set_load_weight()` directly writes the new weight (3) onto `se->load` but skips `reweight_task()`, so `cfs_rq->load.weight` still includes the old weight (e.g., 1024). The sum of entity weights (which now includes 3) no longer matches `cfs_rq->load.weight` (which still reflects 1024).

**Other bugs caught:** Potentially `fair_reweight_entity_null_cfsrq` and any future bug where entity weight changes bypass the cfs_rq load update path (e.g., in group scheduling reweight, throttle/unthrottle weight manipulation, or enqueue/dequeue load accounting errors).
