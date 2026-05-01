# Core Tree Membership Consistent with Cookie and Runqueue State
**Source bug:** `91caa5ae242465c3ab9fd473e50170faa7e944f4`

**Property:** If a task has a non-zero `core_cookie` and is queued on a runqueue (`task_on_rq_queued`), then it must be present in that runqueue's core scheduling rb-tree (`sched_core_enqueued`).

**Variables:**
- `p->core_cookie` — the task's core scheduling cookie value. Read in-place from `task_struct` at check time.
- `task_on_rq_queued(p)` — whether the task is on a runqueue. Read in-place via `p->on_rq == TASK_ON_RQ_QUEUED`.
- `sched_core_enqueued(p)` — whether the task is in the core tree. Read in-place via `!RB_EMPTY_NODE(&p->core_node)`.
- `sched_core_enabled(rq)` — whether core scheduling is active. Read in-place. The invariant only applies when core scheduling is enabled.

**Check(s):**

Check 1: Performed after `sched_core_update_cookie()` returns (or equivalently, after any path that modifies `p->core_cookie`). Precondition: `sched_core_enabled(rq)`.
```c
// After cookie update completes, with rq lock held:
if (sched_core_enabled(rq) && p->core_cookie && task_on_rq_queued(p)) {
    WARN_ON_ONCE(!sched_core_enqueued(p));
}
```

Check 2: Performed at `enqueue_task()` return / `sched_core_enqueue()` exit. Precondition: task is being enqueued onto a core-scheduling-enabled rq.
```c
// After enqueue_task() completes:
if (sched_core_enabled(rq) && p->core_cookie && task_on_rq_queued(p)) {
    WARN_ON_ONCE(!sched_core_enqueued(p));
}
```

Check 3: Performed at `dequeue_task()` return when dequeue is "failed" (delayed dequeue). Precondition: `dequeue_task()` returned `false` (task remains on rq).
```c
// After dequeue_task() returns false (delayed dequeue):
if (sched_core_enabled(rq) && p->core_cookie && task_on_rq_queued(p)) {
    WARN_ON_ONCE(!sched_core_enqueued(p));
}
```

**Example violation:** In the source bug, `sched_core_update_cookie()` sets a non-zero cookie on a task that is on the runqueue but was previously uncookied. The stale `enqueued` boolean (captured before the cookie change) gates the `sched_core_enqueue()` call, so the task ends up with a cookie and on the rq but not in the core tree — violating the invariant.

**Other bugs caught:** `c662e2b1e8cfc3b6329704dab06051f8c3ec2993` (delayed dequeue removes a cookied task from the core tree via `sched_core_dequeue()` before the CFS class decides to keep the task on the runqueue, leaving it on-rq with a cookie but missing from the core tree).
