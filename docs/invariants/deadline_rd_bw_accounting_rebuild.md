# Root Domain DL Bandwidth Accounting Consistency
**Source bug:** `2ff899e3516437354204423ef0a94994717b8e6a`

**Property:** For every root domain, `rd->dl_bw.total_bw` must equal the sum of `dl_bw` of all SCHED_DEADLINE tasks assigned to the domain plus the `dl_bw` of all active dl_servers on CPUs in the domain.

**Variables:**
- `rd->dl_bw.total_bw` — the root domain's tracked total DEADLINE bandwidth. Read directly from `struct root_domain`. Checked in-place after domain rebuild completes.
- `expected_total_bw` — the recomputed sum of all DL entity bandwidths in the domain. Computed by iterating all CPUs in `rd->span`: for each CPU, sum the `dl_bw` of its active `fair_server` dl_server, then iterate all SCHED_DEADLINE tasks (via `css_task_iter` over cpusets or by scanning `rq->dl.pushable_dl_tasks_root` and current DL task) and sum their `p->dl.dl_bw`.

**Check(s):**

Check 1: Performed at the end of `partition_sched_domains_locked()`, after `dl_rebuild_rd_accounting()` completes. Precondition: `sched_domains_mutex` is held; domain configuration is stable.
```c
// For each unique root domain (use dl_bw_visited cookie to visit each rd once):
u64 cookie = ++dl_cookie;
for_each_possible_cpu(cpu) {
    if (dl_bw_visited(cpu, cookie))
        continue;
    struct root_domain *rd = cpu_rq(cpu)->rd;
    u64 expected = 0;

    // Sum dl_server bandwidth for active CPUs in this rd
    for_each_cpu(i, rd->span) {
        struct sched_dl_entity *dl_se = &cpu_rq(i)->fair_server;
        if (dl_server(dl_se) && cpu_active(i))
            expected += dl_se->dl_bw;
    }

    // Sum SCHED_DEADLINE task bandwidth (tasks assigned to CPUs in this rd)
    // (requires iterating DL tasks, e.g., via for_each_process or cpuset iteration)
    for_each_process_thread(g, p) {
        if (dl_task(p) && cpu_isset(task_cpu(p), *rd->span))
            expected += p->dl.dl_bw;
    }

    WARN_ON_ONCE(rd->dl_bw.total_bw != expected);
}
```

**Example violation:** On the buggy kernel, `partition_sched_domains(1, NULL, NULL)` called from the suspend/resume path rebuilds root domains but never calls `dl_rebuild_rd_accounting()`. The new root domain's `total_bw` is initialized to 0 and never has DL task or dl_server bandwidth re-added, so `total_bw < expected_total_bw`.

**Other bugs caught:** Potentially `deadline_extra_bw_stale_rebuild` (stale `extra_bw` during rebuild is a related accounting inconsistency in the same rebuild path).
