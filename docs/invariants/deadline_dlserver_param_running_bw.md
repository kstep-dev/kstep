# DL running_bw Consistency with Entity dl_bw
**Source bug:** `bb4700adc3abec34c0a38b64f66258e4e233fc16`

**Property:** `dl_rq->running_bw` must equal the sum of `dl_se->dl_bw` for all deadline entities that are actively contending (on-rq, not `dl_non_contending`, not throttled) on that runqueue.

**Variables:**
- `running_bw` — the per-runqueue aggregate running bandwidth tracked in `dl_rq->running_bw`. Read directly from `struct dl_rq`. This is a running sum maintained by `__add_running_bw()` / `__sub_running_bw()`.
- `expected_running_bw` — computed by walking all deadline entities on the runqueue and summing `dl_se->dl_bw` for each entity where `dl_se->dl_non_contending == 0` and the entity is enqueued (on the dl_rq rb-tree or is the running dl entity). Computed at check time by iterating the dl_rq.

**Check(s):**

Check 1: Performed after `dl_server_apply_params()` returns. Only when `dl_server_apply_params()` is called with `init == 0` (parameter change, not first-time init).
```c
// After dl_server_apply_params() in sched_fair_server_write():
u64 expected = 0;
struct rb_node *node;
struct sched_dl_entity *entry;

// Sum dl_bw for all contending DL entities on this rq
for (node = rb_first(&rq->dl.root); node; node = rb_next(node)) {
    entry = rb_entry(node, struct sched_dl_entity, rb_node);
    if (!entry->dl_non_contending)
        expected += entry->dl_bw;
}
// Also account for the currently running dl entity if not on the tree
if (rq->dl.curr && !rq->dl.curr->dl_non_contending) {
    // Check if curr is not already counted (not on rb-tree)
    expected += rq->dl.curr->dl_bw;  // may need dedup with tree
}

WARN_ON_ONCE(rq->dl.running_bw != expected);
```

Check 2: Performed at `dequeue_dl_entity()` exit, after running_bw adjustments. Always.
```c
// Same summation as Check 1
// WARN_ON_ONCE(dl_rq->running_bw != expected_running_bw);
```

Check 3: Performed at `enqueue_dl_entity()` exit, after running_bw adjustments. Always.
```c
// Same summation as Check 1
// WARN_ON_ONCE(dl_rq->running_bw != expected_running_bw);
```

**Example violation:** The bug changes `dl_se->dl_bw` via `dl_server_apply_params()` while the dl_server is still contending (active, not `dl_non_contending`), but `running_bw` is not updated to reflect the new `dl_bw`. After the call, `running_bw` holds the old bandwidth contribution while `dl_se->dl_bw` holds the new value, so the sum check fails.

**Other bugs caught:** Potentially catches any bug where `running_bw` drifts from the true sum of contending entities' bandwidth — including other paths that modify `dl_bw` without proper dequeue/re-enqueue, or missing `add_running_bw`/`sub_running_bw` calls.
