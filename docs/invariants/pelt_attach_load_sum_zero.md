# PELT avg/sum Consistency
**Source bug:** `40f5aa4c5eaebfeaca4566217cb9c468e28ed682`

**Property:** For any `cfs_rq` or `sched_entity`, if a PELT `_avg` field is non-zero then the corresponding `_sum` field must also be non-zero.

**Variables:**
- `cfs_rq->avg.load_avg` — aggregated load average of the cfs_rq. Read in-place at check points.
- `cfs_rq->avg.load_sum` — aggregated load sum of the cfs_rq. Read in-place at check points.
- `cfs_rq->avg.util_avg` / `cfs_rq->avg.util_sum` — same pair for utilization.
- `cfs_rq->avg.runnable_avg` / `cfs_rq->avg.runnable_sum` — same pair for runnable.
- `se->avg.load_avg` / `se->avg.load_sum` — per-entity load pair. Read in-place.

No shadow variables needed; all fields are read directly from the structs.

**Check(s):**

Check 1: Performed at the end of `attach_entity_load_avg()`. Always.
```c
SCHED_WARN_ON(se->avg.load_avg && !se->avg.load_sum);
SCHED_WARN_ON(se->avg.util_avg && !se->avg.util_sum);
SCHED_WARN_ON(se->avg.runnable_avg && !se->avg.runnable_sum);
```

Check 2: Performed at `cfs_rq_is_decayed()` (already exists in-tree as of v5.14). When all `_sum` fields are zero.
```c
if (!cfs_rq->avg.load_sum && !cfs_rq->avg.util_sum && !cfs_rq->avg.runnable_sum) {
    SCHED_WARN_ON(cfs_rq->avg.load_avg ||
                  cfs_rq->avg.util_avg ||
                  cfs_rq->avg.runnable_avg);
}
```

Check 3: Performed at the end of `dequeue_load_avg()`. Always.
```c
SCHED_WARN_ON(cfs_rq->avg.load_avg && !cfs_rq->avg.load_sum);
```

Check 4: Performed at the end of `enqueue_load_avg()`. Always.
```c
SCHED_WARN_ON(cfs_rq->avg.load_avg && !cfs_rq->avg.load_sum);
```

**Example violation:** `attach_entity_load_avg()` computes `load_sum = load_avg * divider / se_weight`, which truncates to 0 when `se_weight > load_avg * divider` (e.g., nice -20, load_avg=1). The cfs_rq then has load_avg=1 but load_sum=0.

**Other bugs caught:**
- `7c7ad626d9a0` — `update_tg_cfs_load()` independently updates load_sum and load_avg with `add_positive()` clamping, causing load_sum=0 while load_avg>0.
- `ceb6ba45dc80` — `dequeue_load_avg()` independently subtracts with `sub_positive()`, clamping load_sum to 0 while load_avg remains positive.
