# Non-decayed unthrottled cfs_rq must be on the leaf list
**Source bug:** `a7b359fc6a37faaf472125867c8dc5a068c90982`

**Property:** If a cfs_rq is not throttled (throttle_count == 0) and has non-decayed PELT state or running entities, it must be present on the rq's leaf_cfs_rq_list.

**Variables:**
- `cfs_rq->throttle_count` — number of throttled ancestors (including self). Read in-place at check time. Zero means this cfs_rq is not under any throttled hierarchy.
- `cfs_rq->on_list` — whether this cfs_rq is currently on the leaf_cfs_rq_list. Read in-place at check time.
- `cfs_rq->nr_running` — number of runnable entities on this cfs_rq. Read in-place at check time.
- `cfs_rq_is_decayed(cfs_rq)` — returns true iff `cfs_rq->load.weight == 0 && cfs_rq->avg.load_sum == 0 && cfs_rq->avg.util_sum == 0 && cfs_rq->avg.runnable_sum == 0`. Evaluated in-place at check time.

**Check(s):**

Check 1: Performed at exit of `unthrottle_cfs_rq()`, after all `tg_unthrottle_up()` callbacks and entity enqueue have completed. Only applies to CONFIG_FAIR_GROUP_SCHED && CONFIG_SMP.
```c
// For every cfs_rq in the task group hierarchy on this CPU:
for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos) { /* already on list — ok */ }

// The real check: walk all cfs_rqs in the hierarchy and verify none are missing
struct task_group *tg;
list_for_each_entry_rcu(tg, &task_groups, list) {
    struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];
    if (cfs_rq->throttle_count == 0 &&
        (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running) &&
        !cfs_rq->on_list) {
        // INVARIANT VIOLATION: non-decayed, unthrottled cfs_rq is missing
        // from the leaf list — its PELT averages will never decay.
        WARN_ONCE(1, "cfs_rq not on leaf list but has undecayed load");
    }
}
```

Check 2: Performed periodically in `update_blocked_averages()` / `__update_blocked_fair()`, as a sanity check that no cfs_rq with stale load is orphaned from the leaf list.
```c
// Same predicate as Check 1, evaluated at the start of update_blocked_averages()
struct task_group *tg;
list_for_each_entry_rcu(tg, &task_groups, list) {
    struct cfs_rq *cfs_rq = tg->cfs_rq[cpu];
    if (cfs_rq->throttle_count == 0 &&
        (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running) &&
        !cfs_rq->on_list) {
        WARN_ONCE(1, "cfs_rq with undecayed load missing from leaf list");
    }
}
```

**Example violation:** After `unthrottle_cfs_rq()`, a descendant cfs_rq has `nr_running == 0` but non-zero `avg.load_sum` (from a recently blocked task). The buggy code only checks `nr_running >= 1`, so this cfs_rq is not re-added to the leaf list. Its stale PELT values never decay, corrupting the parent task group's load average and causing 99/1 CPU fairness splits between equal-weight siblings.

**Other bugs caught:**
- `fdaba61ef8a268d4136d0a113d153f7a89eb9984` — decayed parent cfs_rq skipped on leaf list during unthrottle (same invariant: parent has a child on the list but is not itself on the list, violating leaf list completeness for non-throttled cfs_rqs with dependent children)
- `2630cde26711dab0d0b56a8be1616475be646d13` — missing ancestor cfs_rq on leaf list during unthrottle (early return bypasses ancestor addition, leaving unthrottled cfs_rqs off the leaf list)
