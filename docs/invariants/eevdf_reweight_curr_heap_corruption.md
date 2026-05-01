# CFS min_deadline Augmented Heap Invariant
**Source bug:** `d2929762cc3f85528b0ca12f6f63c2a714f24778`

**Property:** For every node in the CFS rb-tree (`cfs_rq->tasks_timeline`), its `min_deadline` must equal the minimum of its own `deadline` and the `min_deadline` values of its left and right children.

**Variables:**
- `se->min_deadline` — the cached minimum deadline for the subtree rooted at this entity. Read in-place from each `sched_entity` in the rb-tree during the check.
- `se->deadline` — the entity's own deadline. Read in-place.
- `left->min_deadline`, `right->min_deadline` — the `min_deadline` of the left/right child nodes. Read in-place from child `sched_entity` via `rb_entry()`.

**Check(s):**

Check 1: Performed after `reweight_entity()` returns. Only when `cfs_rq->nr_running > 0`.
```c
// Walk every node in the CFS rb-tree and verify the augmented heap property.
struct rb_node *node;
for (node = rb_first_cached(&cfs_rq->tasks_timeline); node; node = rb_next(node)) {
    struct sched_entity *se = rb_entry(node, struct sched_entity, run_node);
    u64 expected = se->deadline;

    if (node->rb_left) {
        struct sched_entity *left = rb_entry(node->rb_left, struct sched_entity, run_node);
        if (left->min_deadline < expected)
            expected = left->min_deadline;
    }
    if (node->rb_right) {
        struct sched_entity *right = rb_entry(node->rb_right, struct sched_entity, run_node);
        if (right->min_deadline < expected)
            expected = right->min_deadline;
    }
    WARN_ON_ONCE(se->min_deadline != expected);
}
```

Check 2: Performed at `pick_eevdf()` entry or `__pick_next_entity()`. When `cfs_rq->nr_running > 0`. Same check as above — verifying heap integrity before the scheduler relies on `min_deadline` to find the earliest-deadline eligible entity.

**Example violation:** `reweight_entity()` calls `min_deadline_cb_propagate()` on `cfs_rq->curr`, which is not in the rb-tree. The propagation walks stale parent pointers and writes garbage `min_deadline` values into the tree, breaking the heap property for one or more nodes.

**Other bugs caught:** Likely also catches the class of bugs represented by `eevdf_reweight_min_deadline_heap` and any future bug where `min_deadline` propagation is missed or applied incorrectly after deadline/weight changes.
