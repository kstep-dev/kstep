# Uclamp Bucket ID Within Bounds
**Source bug:** `6d2f8909a5fabb73fe2a63918117943986c39b6c`

**Property:** A task's uclamp bucket_id must always be a valid index into the per-rq bucket array, i.e., in range [0, UCLAMP_BUCKETS - 1].

**Variables:**
- `bucket_id` — the bucket index stored in `p->uclamp_req[clamp_id].bucket_id` or `p->uclamp[clamp_id].bucket_id`. Recorded at `uclamp_se_set()` (after assignment). Read directly from the `struct uclamp_se`.
- `UCLAMP_BUCKETS` — compile-time constant for the number of buckets. Available as a macro.

**Check(s):**

Check 1: Performed at `uclamp_rq_inc_id()` / `uclamp_rq_dec_id()`, before using `bucket_id` to index into `rq->uclamp[clamp_id].bucket[]`.
```c
unsigned int bucket_id = uc_se->bucket_id;
WARN_ON_ONCE(bucket_id >= UCLAMP_BUCKETS);
```

Check 2: Performed at `uclamp_se_set()`, after `bucket_id` is computed and stored.
```c
uc_se->bucket_id = uclamp_bucket_id(value);
WARN_ON_ONCE(uc_se->bucket_id >= UCLAMP_BUCKETS);
```

**Example violation:** With `UCLAMP_BUCKETS=20`, `uclamp_bucket_id(1024)` returns `1024/51 = 20`, which equals `UCLAMP_BUCKETS` and is one past the valid range, causing an out-of-bounds array access when the task is enqueued.

**Other bugs caught:** Potentially `uclamp_rq_init_size_mismatch` (if bucket array sizing is wrong) and any future regression in bucket_id arithmetic.
