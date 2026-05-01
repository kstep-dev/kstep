# Delay-Dequeued Group Entity Weight Preservation
**Source bug:** `66951e4860d3c688bfa550ea4a19635b57e00eca`

**Property:** A group sched_entity that is delay-dequeued (on the runqueue with `sched_delayed` set) must not have its `load.weight` changed.

**Variables:**
- `se->sched_delayed` — whether the entity is currently delay-dequeued. Read in-place from the sched_entity at check time.
- `se->load.weight` — the entity's current load weight. Read in-place at check time; compared against a shadow copy taken when the entity enters the delayed state.
- `saved_weight` — shadow copy of `se->load.weight` recorded when `se->sched_delayed` transitions from 0 to 1 (i.e., at the point `dequeue_entity()` decides to delay the dequeue). Stored per-entity in a shadow structure.

**Check(s):**

Check 1: Performed at `reweight_entity()` entry. Precondition: entity is a group entity (`group_cfs_rq(se) != NULL`).
```c
// At the entry of reweight_entity(cfs_rq, se, weight):
if (group_cfs_rq(se) && se->sched_delayed) {
    // A delay-dequeued group entity should not be reweighted.
    // Its weight must be preserved so it burns off lag at the
    // original competitive rate.
    WARN_ONCE(1, "reweight_entity called on delay-dequeued group se");
}
```

Check 2: Performed at `update_cfs_group()` return / after any weight-modifying path. Precondition: entity has `sched_delayed` set.
```c
// After any call to update_cfs_group(se) or reweight_entity():
if (se->sched_delayed && se->load.weight != saved_weight) {
    // Weight was modified while entity was in delayed state
    WARN_ONCE(1, "delay-dequeued group se weight changed: %lu -> %lu",
              saved_weight, se->load.weight);
}
```

**Example violation:** When a group's child cfs_rq becomes empty and the group entity is delay-dequeued, `update_cfs_group()` calls `calc_group_shares()` which computes MIN_SHARES (2) for the empty group, then `reweight_entity()` changes the weight from ~64 to 2, violating the invariant. The lag is inflated by ~32x, corrupting `avg_vruntime()`.

**Other bugs caught:** None known, but this invariant would catch any future code path that inadvertently reweights a delay-dequeued group entity (e.g., cgroup weight changes propagated without checking delayed state, or new callers of `reweight_entity`).
