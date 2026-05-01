# Task CPU Affinity Consistency
**Source bug:** `751d4cbc43879229dbc124afefe240b70fd29a85`

**Property:** When a task is activated on a runqueue (enqueued after wakeup), the target CPU must be in the task's `cpus_ptr` affinity mask.

**Variables:**
- `cpu` — the CPU on which the task is being activated. Read directly from the `cpu` parameter passed to `ttwu_do_activate()` (or equivalently `task_cpu(p)` after activation).
- `p->cpus_ptr` — the task's current allowed CPU mask. Read directly from the task struct at activation time. Stable because `p->pi_lock` is held during `try_to_wake_up()`.

**Check(s):**

Check 1: Performed at `ttwu_do_activate()`. Applies to all task wakeup activations.
```c
// At the entry of ttwu_do_activate(struct rq *rq, struct task_struct *p, ...)
// rq->cpu is the CPU where p is about to be enqueued.
WARN_ON_ONCE(!cpumask_test_cpu(cpu_of(rq), p->cpus_ptr));
```

Check 2: Performed at `sched_ttwu_pending()`, after processing each task from the wake_list. This specifically covers the IPI-based wakelist path that this bug exploits.
```c
// Inside sched_ttwu_pending(), after llist_for_each_entry_safe
// and before calling ttwu_do_activate:
WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(), p->cpus_ptr));
```

**Example violation:** The ttwu fast path queues a task on a CPU's wake_list without checking the task's affinity mask. When `cpus_ptr` was updated to exclude that CPU (via `set_cpus_allowed_ptr`), the IPI-delivered wakeup activates the task on a disallowed CPU, violating the invariant.

**Other bugs caught:** Potentially `core_deferred_migration_dest` (if a deferred migration selects a CPU outside `cpus_ptr`) and any future bug where a wakeup or migration path bypasses affinity validation.
