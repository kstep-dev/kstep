# EEVDF Pick Returns Earliest Eligible Deadline
**Source bug:** `b01db23d5923a35023540edc4f0c5f019e11ac7d`

**Property:** The entity returned by `pick_eevdf()` must have the minimum deadline among all eligible entities on the cfs_rq (including curr if eligible).

**Variables:**
- `picked_se` — the sched_entity returned by `pick_eevdf()`. Recorded at `pick_next_entity` / `set_next_entity`. Read from the return value of the pick path.
- `eed_se` — the eligible entity with the earliest deadline, computed by brute-force O(n) scan of the rb-tree plus curr. Computed at the same hook point by iterating `cfs_rq->tasks_timeline` with `rb_first()`/`rb_next()` and checking `entity_eligible(cfs_rq, se)` for each, tracking the minimum `se->deadline` via signed comparison.

**Check(s):**

Check 1: Performed at `set_next_entity` (or equivalently after `pick_next_entity` returns). Precondition: `cfs_rq->nr_running > 0`.
```c
// Brute-force scan for earliest eligible deadline
struct sched_entity *eed = NULL;
struct rb_node *node;

// Check curr if eligible
if (cfs_rq->curr && cfs_rq->curr->on_rq &&
    entity_eligible(cfs_rq, cfs_rq->curr)) {
    eed = cfs_rq->curr;
}

// Walk the entire rb-tree
for (node = rb_first_cached(&cfs_rq->tasks_timeline); node; node = rb_next(node)) {
    struct sched_entity *se = rb_entry(node, struct sched_entity, run_node);
    if (!entity_eligible(cfs_rq, se))
        continue;
    if (!eed || (s64)(se->deadline - eed->deadline) < 0)
        eed = se;
}

// The picked entity must match the brute-force result
// (ignoring RUN_TO_PARITY fast path which legitimately returns curr early)
if (eed && picked_se && picked_se != eed) {
    if ((s64)(picked_se->deadline - eed->deadline) > 0) {
        // BUG: picked an entity with a later deadline than the true EED
        WARN(1, "pick_eevdf missed EED: picked deadline=%lld, optimal=%lld",
             picked_se->deadline, eed->deadline);
    }
}
```

**Example violation:** The buggy single-pass `pick_eevdf()` descends right chasing `min_deadline` into an ineligible subtree, then cannot backtrack to a left subtree containing an eligible entity with an earlier deadline. The brute-force scan finds that entity, revealing the mismatch.

**Other bugs caught:** Any future bug in the `__pick_eevdf()` tree search logic that causes it to return a suboptimal eligible entity — e.g., errors in the augmented heap descent, incorrect eligibility filtering, or breakage from tree rebalancing.
