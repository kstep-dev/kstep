# DL Entity No Double Enqueue
**Source bug:** `b53127db1dbf7f1047cf35c10922d801dcd40324`

**Property:** A `sched_dl_entity` that is already on the DL runqueue must not be enqueued again.

**Variables:**
- `on_rq` — whether the dl entity's rb_node is linked into `dl_rq->root`. Read in-place from `dl_se->rb_node` (non-null means on-rq) at the point of enqueue. No shadow variable needed.

**Check(s):**

Check 1: Performed at entry to `__enqueue_dl_entity()`. No preconditions.
```c
// on_dl_rq() checks !RB_EMPTY_NODE(&dl_se->rb_node)
if (on_dl_rq(dl_se)) {
    // VIOLATION: entity is already on the DL runqueue rb-tree.
    // Inserting it again corrupts the rb-tree.
    BUG();
}
```

**Example violation:** The dl_server is stopped (dequeued) during `server_pick_task()` due to delayed dequeue removing the last CFS task, but `__pick_task_dl()` proceeds to call `update_curr_dl_se()` which re-enqueues it. A subsequent `dl_server_start()` then enqueues it a second time, with `on_dl_rq()` already true at the second `__enqueue_dl_entity()` call.

**Other bugs caught:** Any future bug where code paths lead to double insertion of a DL entity into `dl_rq->root`, including race conditions between dl_server start/stop and concurrent enqueue paths.
