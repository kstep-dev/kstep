# Leaf CFS RQ List Ancestor Completeness
**Source bug:** `fdaba61ef8a268d4136d0a113d153f7a89eb9984`

**Property:** If a cfs_rq is on rq->leaf_cfs_rq_list, then every ancestor cfs_rq up to the root must also be on the list.

**Variables:**
- `cfs_rq->on_list` — whether this cfs_rq is currently on the leaf list. Read in-place from `struct cfs_rq`. Checked at the verification point.
- `cfs_rq->tg->parent` — the parent task_group, used to walk up the hierarchy. Read in-place from `struct task_group`.
- `parent_cfs_rq` — the parent's per-cpu cfs_rq (`tg->parent->cfs_rq[cpu]`). Derived by walking the task_group hierarchy upward. Read in-place.

**Check(s):**

Check 1: Performed at the end of `unthrottle_cfs_rq()`, after `list_add_leaf_cfs_rq` calls complete (where `assert_list_leaf_cfs_rq` is already called). Precondition: CONFIG_FAIR_GROUP_SCHED is enabled.
```c
// For each cfs_rq on rq->leaf_cfs_rq_list, walk ancestors and verify they are also on the list.
struct cfs_rq *leaf;
list_for_each_entry(leaf, &rq->leaf_cfs_rq_list, leaf_cfs_rq_list) {
    struct task_group *tg = leaf->tg;
    while (tg->parent && tg->parent != &root_task_group) {
        struct cfs_rq *parent_cfs_rq = tg->parent->cfs_rq[cpu_of(rq)];
        SCHED_WARN_ON(!parent_cfs_rq->on_list);
        tg = tg->parent;
    }
}
```

Check 2: Performed at the end of `__update_blocked_fair()`, after the leaf list iteration that may remove decayed cfs_rqs. Same precondition.
```c
// Same check as above — verify no removal left an orphaned child on the list.
struct cfs_rq *leaf;
list_for_each_entry(leaf, &rq->leaf_cfs_rq_list, leaf_cfs_rq_list) {
    struct task_group *tg = leaf->tg;
    while (tg->parent && tg->parent != &root_task_group) {
        struct cfs_rq *parent_cfs_rq = tg->parent->cfs_rq[cpu_of(rq)];
        SCHED_WARN_ON(!parent_cfs_rq->on_list);
        tg = tg->parent;
    }
}
```

**Example violation:** When unthrottling a nested cgroup hierarchy, an intermediate cfs_rq (level3b) is skipped because `cfs_rq_is_decayed()` returns true (zero PELT, zero nr_running), but its child (worker3's cfs_rq) was already added to the leaf list. This leaves the child on the list without its parent ancestor, violating the invariant.

**Other bugs caught:** `2630cde26711dab0d0b56a8be1616475be646d13` (bandwidth_unthrottle_leaf_ancestor — same class: ancestor missing from leaf list during unthrottle), `a7b359fc6a37faaf472125867c8dc5a068c90982` (bandwidth_unthrottle_decayed_cfsrq — the predecessor fix that first attempted to address leaf list completeness during unthrottle), `39f23ce07b9355d05a64ae303ce20d1c4b92b957` (bandwidth_unthrottle_leaf_fixup — incomplete leaf list maintenance in unthrottle_cfs_rq)
