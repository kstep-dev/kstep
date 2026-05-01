# RT Runqueue Membership Consistent with Scheduling Class
**Source bug:** `f558c2b834ec27e75d37b1c860c139e7b7c3a8e4`

**Property:** If a task's RT sched_entity is linked into an RT runqueue list (`on_list` is set), then the task's `sched_class` must be `rt_sched_class`.

**Variables:**
- `on_list` — whether the task's `rt_se` is linked into the RT runqueue's priority list. Read directly from `p->rt.on_list`. Checked in-place (no shadow variable needed).
- `sched_class` — the task's current scheduling class pointer. Read directly from `p->sched_class`. Checked in-place.

**Check(s):**

Check 1: Performed at the end of `__sched_setscheduler()` (after `__setscheduler_prio()` and any enqueue/dequeue), or more generally after any operation that modifies `p->sched_class` while the task is on a runqueue. Precondition: `p->on_rq == TASK_ON_RQ_QUEUED` (task is actively enqueued).
```c
// After sched_class change or enqueue/dequeue in __sched_setscheduler():
if (p->on_rq == TASK_ON_RQ_QUEUED) {
    if (p->rt.on_list)
        BUG_ON(p->sched_class != &rt_sched_class);
}
```

Check 2: Performed at `enqueue_task_rt()` entry. Precondition: task is being enqueued into RT.
```c
// At entry to enqueue_task_rt():
// If the task is already on the RT list, something went wrong —
// it was never properly dequeued before its class changed away and back.
BUG_ON(p->rt.on_list);
```

**Example violation:** The TOCTOU race causes `__setscheduler()` to set `sched_class = fair_sched_class` while the task remains on the RT list (`on_list` still set) because `DEQUEUE_MOVE` was cleared based on a stale priority computation. Check 1 would catch the inconsistency immediately after the class change.

**Other bugs caught:** This invariant would catch any bug where a task's runqueue list membership diverges from its scheduling class — a general class of errors in scheduling class transitions, not limited to the specific PI race in this commit.
