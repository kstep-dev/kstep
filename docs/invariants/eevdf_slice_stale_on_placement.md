# Entity Slice Matches Sysctl at Placement
**Source bug:** `2f2fc17bab0011430ceb6f2dc1959e7d1f981444`

**Property:** After `place_entity()` completes for a non-custom-slice entity, `se->slice` must equal `sysctl_sched_base_slice`.

**Variables:**
- `se->slice` — the entity's current time slice. Read directly from `struct sched_entity` after `place_entity()` returns.
- `sysctl_sched_base_slice` — the global base slice configuration value. Read directly from the global variable at the same point.
- `se->custom_slice` — whether the entity has a user-specified custom slice (added in later kernels; if not present, all entities are non-custom). Read directly from `struct sched_entity`.

**Check(s):**

Check 1: Performed at the end of `place_entity()` (or equivalently, in `enqueue_entity()` immediately after the `place_entity()` call). Only when the entity does not have a custom slice.
```c
// After place_entity(cfs_rq, se, flags) returns:
if (!se->custom_slice) {
    WARN_ON_ONCE(se->slice != sysctl_sched_base_slice);
}
```

**Example violation:** A task forked before `sched_init_granularity()` scales `sysctl_sched_base_slice` retains the unscaled UP value in `se->slice`. When it is later enqueued via `place_entity()`, the buggy code uses this stale slice to compute `vslice`, producing an incorrectly tight deadline.

**Other bugs caught:** None known.
