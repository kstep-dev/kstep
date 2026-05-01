# CFS RB-Tree Augmented min_slice Consistency
**Source bug:** `563bc2161b94571ea425bbe2cf69fd38e24cdedf`

**Property:** For every `cfs_rq`, the RB-tree root's augmented `min_slice` must equal the minimum `slice` value among all sched_entities currently in that RB-tree.

**Variables:**
- `tree_min_slice` — the `min_slice` field of the RB-tree root node of a `cfs_rq`. Read in-place from `__pick_root_entity(cfs_rq)->min_slice` at the check point. This is the augmented value maintained by `min_vruntime_cb`.
- `true_min_slice` — the actual minimum of `se->slice` across all entities on the RB-tree of the `cfs_rq`. Computed by walking the tree at the check point (only needed for validation, not maintained as shadow state).

**Check(s):**

Check 1: Performed at the end of `enqueue_task_fair()`, after both `for_each_sched_entity()` loops complete. For each `cfs_rq` in the task's ancestor hierarchy (up to root), if the `cfs_rq` has entities on the RB-tree (`cfs_rq->nr_running > 0` and at least one entity is not `cfs_rq->curr`):
```c
// Walk the cfs_rq's rb-tree and compute true minimum slice
struct rb_node *node;
u64 true_min = U64_MAX;
for (node = rb_first_cached(&cfs_rq->tasks_timeline); node; node = rb_next(node)) {
    struct sched_entity *entry = rb_entry(node, struct sched_entity, run_node);
    if (entry->slice < true_min)
        true_min = entry->slice;
}

// Compare with augmented value at the root
struct sched_entity *root_se = __pick_root_entity(cfs_rq);
if (root_se) {
    WARN_ON_ONCE(root_se->min_slice != true_min);
}
```

Check 2: Performed at the end of `dequeue_entities()`, after both `for_each_sched_entity()` loops complete. Same check as above for each `cfs_rq` in the ancestor hierarchy.
```c
// Same tree walk and comparison as Check 1
struct rb_node *node;
u64 true_min = U64_MAX;
for (node = rb_first_cached(&cfs_rq->tasks_timeline); node; node = rb_next(node)) {
    struct sched_entity *entry = rb_entry(node, struct sched_entity, run_node);
    if (entry->slice < true_min)
        true_min = entry->slice;
}

struct sched_entity *root_se = __pick_root_entity(cfs_rq);
if (root_se) {
    WARN_ON_ONCE(root_se->min_slice != true_min);
}
```

**Example violation:** When a task with a small slice is enqueued into a cgroup whose parent group entity is already on the RB-tree, the parent's `se->slice` is updated but `min_vruntime_cb_propagate()` is not called. The RB-tree root's `min_slice` remains at the old (larger) value, while the true minimum across tree nodes is now the smaller value from the updated group entity.

**Other bugs caught:** `bbce3de72be5` (se->slice set to U64_MAX without propagation — same class of augmented tree inconsistency in the min_slice propagation path).
