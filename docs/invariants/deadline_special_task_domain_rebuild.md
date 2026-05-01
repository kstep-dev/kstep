# DL Bandwidth Rebuild Consistency
**Source bug:** `f6147af176eaa4027b692fdbb1a0a60dfaa1e9b6`

**Property:** After a scheduling domain rebuild completes, each root domain's `dl_bw.total_bw` must equal the sum of dl-server contributions plus the per-task bandwidth of only non-special DEADLINE tasks — i.e., the rebuild must apply the same filters as normal admission control.

**Variables:**
- `rd->dl_bw.total_bw` — the root domain's recorded total DEADLINE bandwidth. Read in-place at the end of `partition_sched_domains_locked()`.
- `expected_total_bw` — recomputed sum of bandwidth. Computed by iterating all CPUs in `rd->span`, adding each CPU's `fair_server.dl_bw / nr_cpus`, then iterating all tasks and for each task where `dl_task(p) && !dl_entity_is_special(&p->dl)` and `task_cpu(p)` is in `rd->span`, adding `p->dl.dl_bw / nr_cpus`. This is a snapshot recomputed at check time.

**Check(s):**

Check 1: Performed at the end of `partition_sched_domains_locked()` (after `dl_rebuild_rd_accounting()` and all domain setup is complete). Only when `dl_rebuild_rd_accounting()` was actually invoked during this call.
```c
// For each root domain rd that was just rebuilt:
u64 expected = 0;
int nr_cpus = cpumask_weight(rd->span);

// Add dl-server (fair_server) contributions
for_each_cpu(i, rd->span) {
    struct sched_dl_entity *dl_se = &cpu_rq(i)->fair_server;
    if (dl_se->dl_runtime)
        expected += to_ratio(dl_se->dl_period, dl_se->dl_runtime);
}

// Add non-special DEADLINE task contributions
for_each_process_thread(g, p) {
    if (!dl_task(p))
        continue;
    if (dl_entity_is_special(&p->dl))
        continue;
    if (!cpumask_test_cpu(task_cpu(p), rd->span))
        continue;
    expected += div_u64(p->dl.dl_bw, nr_cpus);
}

WARN_ON_ONCE(rd->dl_bw.total_bw != expected);
```

**Example violation:** On the buggy kernel, `dl_rebuild_rd_accounting()` calls `dl_add_task_root_domain()` for sugov kthreads (special DEADLINE tasks), adding their fake ~10% bandwidth to `total_bw`. The recomputed `expected` excludes these tasks, so `total_bw > expected`, failing the check.

**Other bugs caught:** `2ff899e3516437354204423ef0a94994717b8e6a` (domain rebuild path missing `dl_rebuild_rd_accounting()` entirely, leaving stale `total_bw` that wouldn't match recomputed expected value)
