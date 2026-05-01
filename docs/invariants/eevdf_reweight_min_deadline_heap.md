# CFS rb-tree min_deadline augmented heap consistency
**Source bug:** `8dafa9d0eb1a1550a0f4d462db9354161bc51e0c`

**Property:** For every `sched_entity` node in a `cfs_rq`'s rb-tree, `se->min_deadline` must equal the minimum of `se->deadline` and the `min_deadline` values of its left and right children.

**Variables:**
- `se->min_deadline` — the augmented heap value cached on each rb-tree node. Read in-place from `struct sched_entity`.
- `se->deadline` — the entity's own virtual deadline. Read in-place from `struct sched_entity`.
- `left->min_deadline` / `right->min_deadline` — the `min_deadline` of the left/right children in the rb-tree. Obtained by following `se->run_node.rb_left` / `se->run_node.rb_right` and converting to `sched_entity` via `rb_entry()`. Use `S64_MAX` if the child is NULL.

**Check(s):**

Check 1: Performed at `reweight_entity()` return, or periodically at `scheduler_tick()` / `update_curr()`. Only when `cfs_rq->nr_running > 0`.
```c
// Walk every node in cfs_rq->tasks_timeline rb-tree:
struct rb_node *node;
for (node = rb_first(&cfs_rq->tasks_timeline.rb_root); node; node = rb_next(node)) {
    struct sched_entity *se = rb_entry(node, struct sched_entity, run_node);
    s64 expected = se->deadline;
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
    WARN_ON(se->min_deadline != expected);
}
```

**Example violation:** `reweight_entity()` rescales `se->deadline` in-place while the entity remains in the rb-tree, but fails to call `min_deadline_cb_propagate()`, leaving ancestor nodes with stale `min_deadline` values that no longer reflect the minimum of their subtrees.

**Other bugs caught:** `d2929762cc3f85528b0ca12f6f63c2a714f24778` (same heap property violated via propagation on a `curr` entity whose `run_node` is not actually in the tree, corrupting the heap from stale parent pointers)
