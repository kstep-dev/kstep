# Sched Entity Slice Must Be Bounded
**Source bug:** `bbce3de72be56e4b5f68924b7da9630cc89aa1a8`

**Property:** For any sched_entity on a CFS runqueue, `se->slice` must not exceed a reasonable upper bound (and in particular must never be `U64_MAX`).

**Variables:**
- `se->slice` — the EEVDF time slice assigned to the scheduling entity, in nanoseconds. Read directly from `struct sched_entity`. No shadow variable needed.
- `SLICE_UPPER_BOUND` — a constant upper bound, e.g. `10 * NSEC_PER_SEC` (10 seconds). Any valid slice is orders of magnitude below this; `sysctl_sched_base_slice` defaults to 3ms.

**Check(s):**

Check 1: Performed at `enqueue_entity()` entry. When a sched_entity is being enqueued onto a cfs_rq.
```c
// On enqueue, the slice must be a valid time duration.
if (se->slice > SLICE_UPPER_BOUND) {
    // INVARIANT VIOLATION: se->slice is corrupted
    // (e.g., U64_MAX from cfs_rq_min_slice() on an empty cfs_rq)
}
```

Check 2: Performed at `set_next_entity()`. When an entity is about to become the running entity on its cfs_rq.
```c
if (se->slice > SLICE_UPPER_BOUND) {
    // INVARIANT VIOLATION: corrupted slice about to be used
    // for scheduling decisions (deadline, eligibility, lag calculations)
}
```

Check 3: Performed in the second `for_each_sched_entity()` loop of `dequeue_entities()`, at the `se->slice = slice` assignment. This is the exact point where the bug manifests.
```c
// After: se->slice = slice;
if (se->slice > SLICE_UPPER_BOUND) {
    // INVARIANT VIOLATION: slice propagation produced an invalid value
}
```

**Example violation:** In the buggy `dequeue_entities()`, when a delayed group entity with an empty internal cfs_rq is dequeued and its parent's dequeue is also delayed, `slice` retains the `U64_MAX` sentinel from `cfs_rq_min_slice()` on the empty cfs_rq. This value is then written to the parent's `se->slice`, violating the bound and corrupting all downstream EEVDF calculations (lag, vruntime, eligibility).

**Other bugs caught:** Potentially any bug that corrupts `se->slice` through overflow, uninitialized values, or incorrect propagation in the cgroup hierarchy (e.g., similar issues in `eevdf_cgroup_min_slice_propagation`).
