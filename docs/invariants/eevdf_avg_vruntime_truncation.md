# Freshly Placed Entity Must Be Eligible
**Source bug:** `650cad561cce04b62a8c8e0446b685ef171bc3bb`

**Property:** After `place_entity()` sets a waking/new entity's vruntime, that entity must satisfy `entity_eligible()`.

**Variables:**
- `se->vruntime` — the entity's vruntime after placement. Recorded at `enqueue_entity()`, immediately after `place_entity()` returns. Read directly from the sched_entity struct.
- `cfs_rq->avg_vruntime` — weighted sum of entity keys. Read in-place at check time from the cfs_rq the entity is being enqueued onto.
- `cfs_rq->avg_load` — total weight of runnable entities. Read in-place at check time.
- `cfs_rq->min_vruntime` — current min_vruntime. Read in-place at check time.
- `cfs_rq->curr` — current running entity, needed to include its contribution (same logic as `entity_eligible()`).

**Check(s):**

Check 1: Performed at `enqueue_entity()`, after `place_entity()` has set `se->vruntime` but before the entity is inserted into the rb-tree. Only when this is a wakeup/new-task enqueue (i.e., `ENQUEUE_WAKEUP` or `ENQUEUE_INITIAL` flags set) and `cfs_rq->avg_load > 0`.
```c
// Reproduce the entity_eligible() logic inline:
s64 avg = cfs_rq->avg_vruntime;
long load = cfs_rq->avg_load;

if (cfs_rq->curr && cfs_rq->curr->on_rq) {
    unsigned long weight = scale_load_down(cfs_rq->curr->load.weight);
    avg += entity_key(cfs_rq, cfs_rq->curr) * weight;
    load += weight;
}

s64 key = entity_key(cfs_rq, se);
// Invariant: avg >= key * load
// A freshly placed entity must be eligible.
WARN_ON_ONCE(avg < key * (s64)load);
```

**Example violation:** When `cfs_rq->avg_vruntime` is negative, `avg_vruntime()` uses truncation-toward-zero division which acts as ceiling instead of floor, returning a value slightly above the true average. A task placed at this value has `key * load > avg`, failing the eligibility check.

**Other bugs caught:** Could also catch future bugs in `place_entity()` logic (e.g., incorrect vlag inflation, wrong slice offset) that result in a placed entity being ineligible despite the design intent.
