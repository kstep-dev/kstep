# Leaf CFS RQ List Ancestor Completeness
**Source bug:** `b34cb07dde7c2346dec73d053ce926aeaa087303`

**Property:** For every non-root cfs_rq on rq->leaf_cfs_rq_list, its parent cfs_rq must also be on the leaf_cfs_rq_list.

**Variables:**
- `cfs_rq->on_list` — whether this cfs_rq is currently on the leaf_cfs_rq_list. Read directly from the struct field during list traversal.
- `parent_cfs_rq` — the parent cfs_rq in the cgroup hierarchy, obtained via `cfs_rq->tg->parent->cfs_rq[cpu]` (or equivalent `cfs_rq_of(cfs_rq->tg->se[cpu])`). Read in-place.
- `rq->leaf_cfs_rq_list` — the per-rq linked list of cfs_rqs. Traversed via `for_each_leaf_cfs_rq_safe`.
- `rq->tmp_alone_branch` — temporary pointer tracking incomplete branch insertion. Read directly; must equal `&rq->leaf_cfs_rq_list` when no operation is in progress.

**Check(s):**

Check 1: Performed at the end of `enqueue_task_fair()`, after all three `for_each_sched_entity` loops and before returning. Only when `cfs_bandwidth_used()` is true.
```c
// Walk the leaf list; for each cfs_rq that has a parent task group,
// verify the parent cfs_rq is also on the list.
struct cfs_rq *pos;
list_for_each_entry(pos, &rq->leaf_cfs_rq_list, leaf_cfs_rq_list) {
    struct task_group *tg = pos->tg;
    if (tg->parent && tg->parent != &root_task_group) {
        struct cfs_rq *parent = tg->parent->cfs_rq[cpu_of(rq)];
        SCHED_WARN_ON(!parent->on_list);
    }
}
// Equivalent fast check (already in kernel):
SCHED_WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list);
```

Check 2: Performed at the end of `unthrottle_cfs_rq()`, after entities are re-enqueued. Only when `cfs_bandwidth_used()` is true.
```c
// Same structural check as Check 1.
struct cfs_rq *pos;
list_for_each_entry(pos, &rq->leaf_cfs_rq_list, leaf_cfs_rq_list) {
    struct task_group *tg = pos->tg;
    if (tg->parent && tg->parent != &root_task_group) {
        struct cfs_rq *parent = tg->parent->cfs_rq[cpu_of(rq)];
        SCHED_WARN_ON(!parent->on_list);
    }
}
SCHED_WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list);
```

**Example violation:** During `enqueue_task_fair()`, the first loop adds a child cfs_rq to the leaf list but breaks early because the parent sched_entity is already on_rq. The second loop advances `se` past the parent level due to throttling. The cleanup loop then starts too high in the hierarchy and never adds the parent cfs_rq, leaving a child on the list without its parent — violating ancestor completeness.

**Other bugs caught:** `39f23ce07b9355d05a64ae303ce20d1c4b92b957` (incomplete leaf list in unthrottle_cfs_rq), `c0490bc9bb62d9376f3dd4ec28e03ca0fef97152` (!SMP leaf list assert failure due to missing list_add_leaf_cfs_rq in unthrottle)
