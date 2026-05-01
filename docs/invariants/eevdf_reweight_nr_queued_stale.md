# place_entity() Precondition: nr_queued Excludes Placed Entity
**Source bug:** `c70fc32f44431bb30f9025ce753ba8be25acbba3`

**Property:** When `place_entity(cfs_rq, se, flags)` is called, `cfs_rq->nr_queued` must not count `se` — it must reflect the number of queued entities *excluding* the entity being placed.

**Variables:**
- `cfs_rq->nr_queued` — number of entities logically enqueued on this cfs_rq. Read in-place at the call site of `place_entity()`.
- `se` — the entity being placed. Passed as argument to `place_entity()`.
- `nr_queued_expected` — the count of entities on the rb-tree (via `cfs_rq->nr_queued`) that should exclude `se`. Not a stored variable; verified by checking whether `se` is already accounted for in the count.

**Check(s):**

Check 1: Performed at entry to `place_entity(cfs_rq, se, flags)`. Always.
```c
// Count entities that should be on the tree: all queued entities minus curr if curr==se
// The key property: se should NOT be counted in nr_queued at this point.
// A practical check: if se was on_rq before this placement (i.e., a reweight or
// re-placement path), then nr_queued must have been decremented already.
//
// For the single-entity case (the triggering scenario), if se is the only entity
// and was on_rq, nr_queued must be 0 (not 1).

// Inside place_entity(), at the PLACE_LAG block:
if (sched_feat(PLACE_LAG) && cfs_rq->nr_queued) {
    u64 load = cfs_rq->avg_load;
    if (curr && curr->on_rq)
        load += scale_load_down(curr->load.weight);
    // This WARN fires when nr_queued > 0 but the tree is actually empty
    // (load == 0 means no entities contribute weight — contradicts nr_queued > 0)
    WARN_ON_ONCE(!load);
}
```

Check 2: Performed at entry to `place_entity(cfs_rq, se, flags)`. When `se` was previously `on_rq` (reweight path).
```c
// At the point place_entity() is called during reweight_entity():
// Verify nr_queued was properly decremented.
// If se is the sole entity on cfs_rq, nr_queued must be 0.
// If there are N other entities, nr_queued must be N (not N+1).

// A direct check (requires instrumenting place_entity or reweight_entity):
unsigned int tree_count = count_entities_on_rbtree(cfs_rq); // walk rb-tree
unsigned int expected = tree_count;
if (cfs_rq->curr && cfs_rq->curr->on_rq && cfs_rq->curr != se)
    expected += 1;  // curr is on_rq but not on the tree
WARN_ON_ONCE(cfs_rq->nr_queued != expected);
```

**Example violation:** In the buggy kernel, `reweight_entity()` calls `place_entity()` without first decrementing `cfs_rq->nr_queued`. When the reweighted entity is the sole entity on the cfs_rq, `nr_queued` is 1 instead of 0, causing `place_entity()` to enter the PLACE_LAG block with an empty tree (`avg_load == 0`), producing a `WARN_ON_ONCE` and incorrect lag inflation.

**Other bugs caught:** Potentially any future bug where a new caller of `place_entity()` (e.g., during migration, class switching, or other reweight paths) fails to properly adjust `nr_queued` before placement.
