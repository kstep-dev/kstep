# Task-CPU / Runqueue Consistency on Activation
**Source bug:** `b6e13e85829f032411b896bd2f0d6cbe4b0a3c4a`

**Property:** When a task is activated (enqueued) on a runqueue, `task_cpu(p)` must equal `cpu_of(rq)`.

**Variables:**
- `task_cpu(p)` — the CPU recorded in the task's `p->cpu` field. Read in-place at the activation site. Obtained via `task_cpu(p)`.
- `cpu_of(rq)` — the CPU that owns the runqueue performing the activation. Read in-place from the `rq` being operated on. Obtained via `cpu_of(rq)`.

**Check(s):**

Check 1: Performed at `ttwu_do_activate()` / `sched_ttwu_pending()`, just before calling `activate_task()`. Precondition: `rq->lock` is held.
```c
// Before activating a task on this rq, its cpu must match.
WARN_ON_ONCE(task_cpu(p) != cpu_of(rq));
```

Check 2: Performed at `activate_task()` entry. Precondition: `rq->lock` is held.
```c
WARN_ON_ONCE(task_cpu(p) != cpu_of(rq));
```

**Example violation:** Due to missing memory ordering between loading `p->cpu` and `p->on_cpu` in `try_to_wake_up()`, a stale `task_cpu(p)` value (pointing to the waker's CPU instead of the task's actual CPU) was used to queue the task on the wrong CPU's wakelist. When `sched_ttwu_pending()` activated the task, `task_cpu(p)` pointed to a different CPU than the rq performing the enqueue, causing the task's `se.cfs_rq` to belong to the wrong CPU's hierarchy and a NULL deref in `find_matching_se()`.

**Other bugs caught:** Potentially any bug where a task is migrated or woken up with an inconsistent `p->cpu` value, including other wakelist or migration races (e.g., `core_deferred_migration_dest`).
