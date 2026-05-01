# Leaf-list cfs_rq must belong to a live task group
**Source bug:** `b027789e5e50494c2325cc70c8642e7fd6059479`

**Property:** Every cfs_rq on a per-rq leaf cfs_rq list must belong to a task group that is still reachable from the global `task_groups` list (or is `root_task_group`).

**Variables:**
- `cfs_rq` — the cfs_rq being visited on the leaf list. Read in-place during leaf list iteration at `update_blocked_averages()` or any leaf list walker.
- `cfs_rq->tg` — the task group owning this cfs_rq. Read in-place from the cfs_rq struct.
- `tg_on_list` — whether `cfs_rq->tg` is present in the global `task_groups` list. Determined by walking the `task_groups` list, or more efficiently by maintaining a shadow flag `tg->online` that is set in `sched_online_group()` and cleared in `sched_offline_group()` / `sched_release_group()`.

**Check(s):**

Check 1: Performed at `update_blocked_averages()`, during the `for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)` loop. For each cfs_rq visited:
```c
// Option A: using a shadow 'online' flag on the task group
if (cfs_rq->tg != &root_task_group) {
    WARN_ON_ONCE(!cfs_rq->tg->online);
}

// Option B: walk the task_groups list (expensive, debug-only)
bool found = false;
struct task_group *tg;
list_for_each_entry_rcu(tg, &task_groups, list) {
    if (tg == cfs_rq->tg) { found = true; break; }
}
if (cfs_rq->tg != &root_task_group) {
    WARN_ON_ONCE(!found);
}
```

Check 2: Performed at `list_add_leaf_cfs_rq()`, when a cfs_rq is being added to the leaf list. Before adding:
```c
if (cfs_rq->tg != &root_task_group) {
    WARN_ON_ONCE(!cfs_rq->tg->online);
}
```

**Example violation:** After `unregister_fair_sched_group()` removes a dying tg's cfs_rq from the leaf list, a concurrent `tg_unthrottle_up()` re-adds the cfs_rq via `list_add_leaf_cfs_rq()`. The cfs_rq is now on the leaf list but its task group is dead (no longer in `task_groups`). When `update_blocked_averages()` walks the leaf list, it accesses the freed cfs_rq, triggering a use-after-free.

**Other bugs caught:** Potentially catches any bug where cfs_rq's of dead task groups leak onto the leaf list, including variants introduced by future changes to the unthrottle or cgroup teardown paths.
