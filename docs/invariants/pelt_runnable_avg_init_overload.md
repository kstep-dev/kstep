# New Entity Runnable-Util Consistency
**Source bug:** `e21cf43406a190adfcc4bfe592768066fb3aaa9b`

**Property:** When a scheduling entity is first initialized (before it has ever been on a runqueue), its `runnable_avg` must equal its `util_avg`, because no queuing delay has occurred yet.

**Variables:**
- `sa->runnable_avg` — the entity's PELT runnable average signal. Read directly from `struct sched_avg` at the end of `post_init_entity_util_avg()`.
- `sa->util_avg` — the entity's PELT utilization average signal. Read directly from `struct sched_avg` at the end of `post_init_entity_util_avg()`, after it has been computed.

**Check(s):**

Check 1: Performed at the end of `post_init_entity_util_avg()`, after all PELT signal initialization and before returning. Only applies to fair-class tasks (the function returns early for non-fair tasks before attachment).
```c
// After util_avg and runnable_avg have been initialized, but before
// attach_entity_load_avg propagates the values to the cfs_rq:
WARN_ON_ONCE(sa->runnable_avg != sa->util_avg);
```

Check 2: Performed at `attach_entity_load_avg()` when `se->avg.last_update_time == 0` (indicating a brand-new entity that has never been updated by PELT decay). This catches any path that attaches a new entity.
```c
if (se->avg.last_update_time == 0) {
    // New entity: no scheduling history yet, so no queuing delay.
    // runnable_avg must equal util_avg.
    WARN_ON_ONCE(se->avg.runnable_avg != se->avg.util_avg);
}
```

**Example violation:** The bug set `sa->runnable_avg = cpu_scale` (1024) while `sa->util_avg` was computed to a much smaller value based on current CFS utilization. This violated the invariant because a brand-new task cannot have queuing delay, so its runnable and utilized signals must be equal.

**Other bugs caught:** None known — but this invariant would catch any future regression that initializes PELT runnable and util signals inconsistently for new entities.
