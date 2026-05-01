# Delayed Tasks Must Not Be In Core Tree
**Source bug:** `c662e2b1e8cfc3b6329704dab06051f8c3ec2993`

**Property:** If a task has `se.sched_delayed == 1`, it must not be present in the sched_core rb-tree (`rq->core_tree`).

**Variables:**
- `p->se.sched_delayed` — whether the task's dequeue is delayed (it is conceptually sleeping but still on the CFS runqueue). Read directly from `task_struct` at check time.
- `sched_core_enqueued(p)` — whether the task's `core_node` is linked into the `rq->core_tree` rb-tree. Checked via `!RB_EMPTY_NODE(&p->core_node)` at check time.

**Check(s):**

Check 1: Performed at `sched_core_enqueue()`, after insertion into `core_tree`. Guards: `sched_core_enabled(rq) && p->core_cookie`.
```c
// After rb_add in sched_core_enqueue:
SCHED_WARN_ON(p->se.sched_delayed && sched_core_enqueued(p));
```

Check 2: Performed at `enqueue_task()`, after `sched_core_enqueue()` returns. Guards: `sched_core_enabled(rq)`.
```c
// At end of enqueue_task, after sched_core_enqueue:
SCHED_WARN_ON(p->se.sched_delayed && !RB_EMPTY_NODE(&p->core_node));
```

Check 3: Performed at `dequeue_task()`, after the scheduling class `dequeue_task()` returns. Guards: `sched_core_enabled(rq)`.
```c
// After p->sched_class->dequeue_task() in dequeue_task:
SCHED_WARN_ON(p->se.sched_delayed && sched_core_enqueued(p));
```

**Example violation:** Without the fix, `dequeue_task()` calls `sched_core_dequeue()` before the CFS class sets `sched_delayed=1`. A subsequent `enqueue_task()` (e.g., during migration) then calls `sched_core_enqueue()` which inserts the still-delayed task into `core_tree`, violating the invariant.

**Other bugs caught:** The analogous uclamp bug `dfa0a574cbc4` ("sched/uclamp: Handle delayed dequeue") follows the same pattern — auxiliary accounting not guarded by `sched_delayed` — though it affects different data structures.
