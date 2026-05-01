# RT Priority Bitmap–Queue Consistency
**Source bug:** `7c4a5b89a0b5a57a64b601775b296abf77a9fe97`

**Property:** If a bit is set in `rt_rq->active.bitmap`, the corresponding priority queue must be non-empty.

**Variables:**
- `idx` — the first set bit in `rt_rq->active.bitmap`. Recorded at `pick_next_rt_entity()`. Obtained via `sched_find_first_bit(array->bitmap)`.
- `queue` — the list head at `rt_rq->active.queue[idx]`. Read in-place at `pick_next_rt_entity()`.

**Check(s):**

Check 1: Performed at `pick_next_rt_entity()`, after `sched_find_first_bit()` returns `idx < MAX_RT_PRIO`.
```c
idx = sched_find_first_bit(array->bitmap);
if (idx < MAX_RT_PRIO) {
    queue = array->queue + idx;
    WARN_ON(list_empty(queue));  // bitmap says tasks exist, queue must agree
}
```

Check 2: Performed at `dequeue_rt_entity()`, after removing an entity from its queue. If the queue becomes empty, the corresponding bitmap bit must be cleared.
```c
// After list_del_init(&rt_se->run_list) at priority p:
if (list_empty(&array->queue[p]))
    WARN_ON(test_bit(p, array->bitmap));  // empty queue must have bit cleared
```

**Example violation:** The bitmap indicates runnable tasks at priority `idx`, but `queue[idx]` is empty. `list_entry()` on the empty list returns a bogus pointer into the `rt_prio_array` itself, which is silently used as a `sched_rt_entity`, causing memory corruption.

**Other bugs caught:** None known — this is a structural consistency invariant that guards against any future bitmap/queue desynchronization (e.g., from RT group throttling bugs or race conditions in enqueue/dequeue paths).
