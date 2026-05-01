# Misfit Status Requires Migratable Task
**Source bug:** `0ae78eec8aa64e645866e75005162603a77a0f49`

**Property:** If `rq->misfit_task_load` is non-zero, the currently running task on that rq must be migratable (i.e., `curr->nr_cpus_allowed > 1`).

**Variables:**
- `rq->misfit_task_load` — indicates the rq has a task that doesn't fit its CPU's capacity. Read in-place at check points.
- `rq->curr->nr_cpus_allowed` — number of CPUs the current task is allowed to run on. Read in-place at check points.

**Check(s):**

Check 1: Performed at the end of `update_misfit_status()`. Only when `rq->misfit_task_load != 0`.
```c
// After update_misfit_status() sets rq->misfit_task_load:
if (rq->misfit_task_load != 0) {
    struct task_struct *curr = rq->curr;
    WARN_ON_ONCE(curr && curr->nr_cpus_allowed == 1);
}
```

Check 2: Performed at `check_misfit_status()` / load balancer entry for misfit. Before initiating a misfit-motivated balance.
```c
// When load balancer is triggered due to misfit:
if (rq->misfit_task_load != 0 && rq->curr) {
    WARN_ON_ONCE(rq->curr->nr_cpus_allowed == 1);
}
```

**Example violation:** A task pinned to a single little-core CPU has utilization exceeding that CPU's capacity. `update_misfit_status()` sets `rq->misfit_task_load` despite `nr_cpus_allowed == 1`, violating the invariant. This triggers futile load balance attempts that exponentially inflate `balance_interval`.

**Other bugs caught:** None known, but would catch any future code path that sets misfit status without verifying task migratability.
