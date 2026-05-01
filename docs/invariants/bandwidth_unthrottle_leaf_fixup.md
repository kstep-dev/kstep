# Active cfs_rq Must Be On Leaf List
**Source bug:** `39f23ce07b9355d05a64ae303ce20d1c4b92b957`

**Property:** Every non-throttled cfs_rq with nr_running > 0 must be present on the rq's leaf_cfs_rq_list (i.e., on_list == 1).

**Variables:**
- `cfs_rq->nr_running` — number of sched_entities directly queued on this cfs_rq. Read in-place from the cfs_rq struct.
- `cfs_rq->throttled` — whether this cfs_rq is directly throttled (bandwidth exhausted). Read in-place.
- `cfs_rq->on_list` — whether this cfs_rq is on rq->leaf_cfs_rq_list. Read in-place.

**Check(s):**

Check 1: Performed after `unthrottle_cfs_rq()` completes (just before `assert_list_leaf_cfs_rq`). For each cfs_rq on this rq.
```c
// Walk all cfs_rqs associated with this rq via task groups
for_each_leaf_cfs_rq_safe(rq, cfs_rq) { /* already on list, fine */ }

// The real check: for every cfs_rq on this cpu
// (walk via tg list or rq->cfs_tasks):
struct task_group *tg;
list_for_each_entry_rcu(tg, &task_groups, list) {
    struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];
    if (cfs_rq->nr_running > 0 && !cfs_rq->throttled && !cfs_rq->on_list)
        WARN_ON_ONCE(1); // invariant violated
}
```

Check 2: Performed after `enqueue_task_fair()` completes (just before `assert_list_leaf_cfs_rq`). Same check as above.
```c
struct task_group *tg;
list_for_each_entry_rcu(tg, &task_groups, list) {
    struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];
    if (cfs_rq->nr_running > 0 && !cfs_rq->throttled && !cfs_rq->on_list)
        WARN_ON_ONCE(1); // invariant violated
}
```

**Example violation:** After `unthrottle_cfs_rq()`, the second loop (updating already-on-rq entities) encounters a cfs_rq in a throttled hierarchy but never calls `list_add_leaf_cfs_rq()` to re-add it. The cfs_rq has nr_running > 0, is not itself throttled, yet on_list == 0 — violating the invariant.

**Other bugs caught:**
- `b34cb07dde7c2346dec73d053ce926aeaa087303` — leaf list corruption during enqueue with throttled hierarchy; same missing leaf list addition
- `c0490bc9bb62d9376f3dd4ec28e03ca0fef97152` — !SMP cfs_rq_is_decayed() always true prevents list_add_leaf_cfs_rq from being called during unthrottle
- `2630cde26711dab0d0b56a8be1616475be646d13` — missing ancestor cfs_rq on leaf list during unthrottle when cfs_rq->load.weight == 0 (note: this variant involves a cfs_rq with child on list but itself not on list, caught by a slightly broader form of this invariant checking ancestry)
