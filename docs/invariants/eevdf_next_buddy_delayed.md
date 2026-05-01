# CFS Buddy Pointers Must Not Reference Delayed Entities
**Source bug:** `493afbd187c4c9cc1642792c0d9ba400c3d6d90d`

**Property:** CFS run queue buddy pointers (`cfs_rq->next`, `cfs_rq->last`) must never point to a sched entity with `sched_delayed == 1`.

**Variables:**
- `cfs_rq->next` — the "next buddy" hint for `pick_next_entity()`. Read in-place at check time. Set by `set_next_buddy()`, cleared by `clear_buddies()`.
- `cfs_rq->last` — the "last buddy" hint for `pick_next_entity()`. Read in-place at check time. Set by `set_last_buddy()`, cleared by `clear_buddies()`.
- `se->sched_delayed` — whether the entity is in the delayed-dequeue state (conceptually sleeping but still on the rq). Read in-place from the entity pointed to by the buddy pointer.

**Check(s):**

Check 1: Performed at `pick_next_entity()`, before using buddy pointers. Always applicable when `DELAY_DEQUEUE` feature is enabled.
```c
if (cfs_rq->next && cfs_rq->next->sched_delayed)
    SCHED_WARN_ON(1); // buddy ->next is delayed

if (cfs_rq->last && cfs_rq->last->sched_delayed)
    SCHED_WARN_ON(1); // buddy ->last is delayed
```

Check 2: Performed at `set_next_buddy()` / `set_last_buddy()`, at the point where the buddy is being assigned. Catches violations at the source.
```c
// In set_next_buddy(), before setting cfs_rq->next = se:
if (se->sched_delayed)
    SCHED_WARN_ON(1); // attempting to set delayed entity as next buddy
```

Check 3: Performed at `dequeue_entity()`, after marking `se->sched_delayed = 1`. Verifies no buddy pointer still references this entity.
```c
// After se->sched_delayed = 1:
if (cfs_rq->next == se || cfs_rq->last == se)
    SCHED_WARN_ON(1); // delayed entity is still a buddy
```

**Example violation:** `check_preempt_wakeup_fair()` calls `set_next_buddy(pse)` on a delayed entity migrated via load balancing, setting `cfs_rq->next` to point to an entity with `sched_delayed == 1`. When `pick_next_entity()` later uses this buddy, it picks a conceptually-sleeping entity to run, causing accounting corruption and potential NULL dereference panics.

**Other bugs caught:** None known, but this invariant would catch any future code path that nominates a delayed entity as a scheduling buddy (e.g., new buddy-setting call sites added without delayed-dequeue awareness).
