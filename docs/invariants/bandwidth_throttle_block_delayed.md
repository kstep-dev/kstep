# Delayed Dequeue Must Block Task
**Source bug:** `e67e3e738f088e6c5ccfab618a29318a3f08db41`

**Property:** After `dequeue_task()` returns for a task dequeued with `DEQUEUE_DELAYED`, the task must be blocked (`p->on_rq == 0`), regardless of whether the dequeue traversal was cut short by a throttled hierarchy or completed normally.

**Variables:**
- `flags` — dequeue flags passed to `dequeue_task()`. Recorded at `dequeue_task_fair()` entry. Read directly from the `flags` parameter.
- `task_delayed_before` — whether `se->sched_delayed` was true before the dequeue. Recorded at `dequeue_task_fair()` entry. Read from `p->se.sched_delayed`.
- `on_rq_after` — the task's `on_rq` value after dequeue completes. Recorded at `dequeue_task_fair()` return. Read from `p->on_rq`.

**Check(s):**

Check 1: Performed at `dequeue_task_fair()` return. Precondition: `flags & DEQUEUE_DELAYED` was set and `task_delayed_before` was true and `dequeue_entities()` did not return -1 (i.e., the dequeue was not merely a delayed-dequeue deferral).
```c
// At entry to dequeue_task_fair():
bool had_delayed = (flags & DEQUEUE_DELAYED) && p->se.sched_delayed;

// At return from dequeue_task_fair() (after dequeue_entities succeeds, i.e., >= 0):
if (had_delayed) {
    WARN_ON_ONCE(p->on_rq != 0);
}
```

**Example violation:** On the buggy kernel, `dequeue_entities()` hits a throttled `cfs_rq` and executes `return 0`, skipping the `__block_task()` call. The task's `p->on_rq` remains 1 despite being removed from the scheduling hierarchy, violating the invariant. `wait_task_inactive()` then loops forever because `task_on_rq_queued()` never returns false.

**Other bugs caught:** Potentially any future bug where a new early-return path is added to `dequeue_entities()` that skips the `__block_task()` epilogue for delayed tasks.
