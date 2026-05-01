# DL Bandwidth Consistency: extra_bw vs total_bw
**Source bug:** `fcc9276c4d331cd1fe9319d793e80b02e09727f5`

**Property:** The sum of reserved bandwidth across all CPUs in a root domain (derived from per-CPU `extra_bw`) must equal the domain-wide `total_bw`.

**Variables:**
- `rd->dl_bw.total_bw` — total DL bandwidth reserved across the root domain. Read directly from `struct root_domain` at check time.
- `cpu_rq(i)->dl.extra_bw` — spare reclaimable bandwidth on CPU i. Read directly from `struct dl_rq` at check time. The reserved bandwidth on CPU i is `max_bw - extra_bw`.
- `cpu_rq(i)->dl.max_bw` — maximum reclaimable bandwidth on CPU i (constant per CPU). Read directly from `struct dl_rq`.
- `rd->span` — cpumask of CPUs in the root domain. Read from `struct root_domain`.

**Check(s):**

Check 1: Performed at the end of `dl_clear_root_domain()`, after all `__dl_add()` calls complete. Precondition: `rd->dl_bw.lock` is held.
```c
// sum_reserved = sum over all CPUs in rd->span of (max_bw_i - extra_bw_i)
u64 sum_reserved = 0;
int i;
for_each_cpu(i, rd->span) {
    struct rq *rq = cpu_rq(i);
    /* extra_bw must not exceed max_bw (would indicate underflow) */
    WARN_ON_ONCE(rq->dl.extra_bw > rq->dl.max_bw);
    sum_reserved += rq->dl.max_bw - rq->dl.extra_bw;
}
WARN_ON_ONCE(sum_reserved != rd->dl_bw.total_bw);
```

Check 2: Performed at the end of `dl_add_task_root_domain()`, after `__dl_add()` for a task. Precondition: `rd->dl_bw.lock` is held.
```c
// Same sum check as above
u64 sum_reserved = 0;
int i;
for_each_cpu(i, rq->rd->span) {
    struct rq *cpu = cpu_rq(i);
    WARN_ON_ONCE(cpu->dl.extra_bw > cpu->dl.max_bw);
    sum_reserved += cpu->dl.max_bw - cpu->dl.extra_bw;
}
WARN_ON_ONCE(sum_reserved != rq->rd->dl_bw.total_bw);
```

**Example violation:** On the buggy kernel, `dl_clear_root_domain()` resets `total_bw` to 0 but leaves `extra_bw` at stale values. After re-adding dl-server bandwidth, `total_bw = T` but `sum(max_bw - extra_bw) = 2T` (double-counted from the previous rebuild), violating the invariant.

**Other bugs caught:** Potentially `deadline_rd_bw_accounting_rebuild` and any future bug where `__dl_add`/`__dl_sub` paths fail to keep per-CPU `extra_bw` and domain-wide `total_bw` in sync.
