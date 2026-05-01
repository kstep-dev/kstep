# h_nr_delayed Must Equal Count of Delayed Task Entities in Hierarchy
**Source bug:** `3429dd57f0deb1a602c2624a1dd7c4c11b6c4734`

**Property:** For any cfs_rq, `h_nr_delayed` must equal the number of sched_entities with `sched_delayed == 1` that are *task* entities (not group entities) in the hierarchy rooted at that cfs_rq.

**Variables:**
- `cfs_rq->h_nr_delayed` — the kernel's running counter of delayed tasks in the hierarchy. Read in-place from the cfs_rq struct at check time.
- `actual_delayed_tasks` — computed by walking all task sched_entities reachable under the cfs_rq and summing those with `se->sched_delayed == 1`. Computed on-the-fly at check time (not stored).

**Check(s):**

Check 1: Performed after `dequeue_task_fair()` returns. Precondition: the rq lock is held (which it is inside the dequeue path); walk each cfs_rq in the dequeued task's ancestor chain.
```c
// For each cfs_rq from the task's immediate cfs_rq up to root:
for_each_sched_entity(se) {
    struct cfs_rq *cfs_rq = cfs_rq_of(se);
    unsigned int counted = count_delayed_tasks(cfs_rq); // walk hierarchy counting task se's with sched_delayed==1
    SCHED_WARN_ON(cfs_rq->h_nr_delayed != counted);
    if (cfs_rq_throttled(cfs_rq))
        break;
}
```

Check 2: Performed after `enqueue_task_fair()` returns. Same structure as Check 1.
```c
for_each_sched_entity(se) {
    struct cfs_rq *cfs_rq = cfs_rq_of(se);
    unsigned int counted = count_delayed_tasks(cfs_rq);
    SCHED_WARN_ON(cfs_rq->h_nr_delayed != counted);
    if (cfs_rq_throttled(cfs_rq))
        break;
}
```

**Example violation:** When a group entity (cfs_rq B) is delayed via `set_delayed()`, the buggy code increments `h_nr_delayed` in all ancestor cfs_rqs despite B not being a task. This causes `h_nr_delayed` to exceed the actual count of delayed task entities, e.g., root cfs_rq shows `h_nr_delayed == 1` when zero task entities are delayed.

**Other bugs caught:** Any future bug that incorrectly adjusts `h_nr_delayed` for non-task entities, or that fails to increment/decrement `h_nr_delayed` when a task entity's `sched_delayed` changes.
