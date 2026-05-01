# Leaf List Ancestor Completeness
**Source bug:** `2630cde26711dab0d0b56a8be1616475be646d13`

**Property:** If a `cfs_rq` is on the per-rq leaf list (`cfs_rq->on_list == 1`), then every ancestor `cfs_rq` up to the root task group must also be on the leaf list.

**Variables:**
- `cfs_rq->on_list` — whether this `cfs_rq` is currently linked into `rq->leaf_cfs_rq_list`. Read directly from the struct at check time.
- `cfs_rq->tg->parent` — parent task group, used to walk up the hierarchy. Read directly at check time.
- `rq->tmp_alone_branch` — tracks an in-progress branch insertion; must equal `&rq->leaf_cfs_rq_list` when no insertion is in progress. Read directly at check time.

**Check(s):**

Check 1: Performed after `unthrottle_cfs_rq()` returns (or equivalently at the `assert_list_leaf_cfs_rq` point). Also after `enqueue_task_fair()` and `sched_move_task()`.
```c
// For each cfs_rq on rq->leaf_cfs_rq_list, verify all ancestors are also on the list.
struct cfs_rq *leaf;
list_for_each_entry(leaf, &rq->leaf_cfs_rq_list, leaf_cfs_rq_elem) {
    struct task_group *tg = leaf->tg;
    int cpu = cpu_of(rq);
    // Walk ancestors
    while (tg->parent && tg->parent != &root_task_group) {
        tg = tg->parent;
        struct cfs_rq *ancestor_cfs_rq = tg->cfs_rq[cpu];
        if (!ancestor_cfs_rq->on_list) {
            // VIOLATION: child on leaf list but ancestor is missing
            WARN(1, "cfs_rq on leaf list but ancestor cfs_rq (tg=%s) missing",
                 tg->css.cgroup ? cgroup_name(tg->css.cgroup) : "?");
        }
    }
}
```

Check 2: Quick sanity check at the same points — `tmp_alone_branch` must be reset.
```c
WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list);
```

**Example violation:** After `walk_tg_tree_from()` adds a descendant `cfs_rq` to the leaf list during unthrottle (because it has undecayed PELT), the function returns early when `cfs_rq->load.weight == 0`, skipping the ancestor-fixup loop. The intermediate ancestor `cfs_rq` (which was fully decayed) is never added to the leaf list, violating ancestor completeness.

**Other bugs caught:**
- `a7b359fc6a37` — missing leaf list re-add for undecayed cfs_rqs during unthrottle
- `fdaba61ef8a2` — decayed parent cfs_rq skipped during unthrottle
- `39f23ce07b93` — incomplete leaf list maintenance in unthrottle_cfs_rq
- `b34cb07dde7c` — leaf list corruption during enqueue with throttled hierarchy
- `c0490bc9bb62` — cfs_rq_is_decayed() always true on !SMP breaks leaf list
