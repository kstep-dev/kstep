# SCHED_IDLE Entity Must Not Be Picked Over Non-Idle Eligible Entities
**Source bug:** `f553741ac8c0e467a3b873e305f34b902e50b86d`

**Property:** When `pick_eevdf()` returns a SCHED_IDLE entity, no non-idle eligible entity may exist on the same cfs_rq.

**Variables:**
- `picked` — the sched_entity returned by `pick_eevdf()`. Read directly from the return value at the end of `pick_eevdf()`.
- `picked_is_idle` — whether the picked entity belongs to a SCHED_IDLE task. Derived via `se_is_idle(picked)` at the check point.
- `cfs_rq` — the CFS runqueue being picked from. Available as the argument to `pick_eevdf()`.

**Check(s):**

Check 1: Performed at the return of `pick_eevdf()`. Only when the returned entity is non-NULL and `se_is_idle(picked)` is true.
```c
// After pick_eevdf() selects 'picked' from 'cfs_rq':
if (picked && se_is_idle(picked)) {
    struct rb_node *node;
    for (node = rb_first_cached(&cfs_rq->tasks_timeline); node; node = rb_next(node)) {
        struct sched_entity *se = __node_2_se(node);
        if (!se_is_idle(se) && entity_eligible(cfs_rq, se)) {
            // VIOLATION: a non-idle eligible entity was skipped
            // in favor of a SCHED_IDLE entity
            WARN_ONCE(1, "pick_eevdf returned idle entity %p "
                      "while non-idle eligible entity %p exists",
                      picked, se);
            break;
        }
    }
}
```

**Example violation:** The SCHED_IDLE current entity has active slice protection (`vlag == deadline`), causing the RUN_TO_PARITY shortcut in `pick_eevdf()` to return it immediately. A recently-woken SCHED_NORMAL entity is eligible on the same cfs_rq but is never considered, violating the invariant.

**Other bugs caught:** None known, but this would catch any future bug where slice protection, eligibility checks, or pick logic incorrectly favors a SCHED_IDLE entity over available non-idle work (e.g., a new fast-path that skips the idle check).
