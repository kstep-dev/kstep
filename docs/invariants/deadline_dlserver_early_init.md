# DL extra_bw consistency with root domain total_bw
**Source bug:** `9f239df55546ee1d28f0976130136ffd1cad0fd7`

**Property:** For every CPU in a root domain, `extra_bw` must equal `max_bw - dl_bw.total_bw / nr_active_cpus`, i.e., the per-CPU extra bandwidth must be consistent with the root domain's total allocated DL bandwidth evenly divided across active CPUs.

**Variables:**
- `extra_bw` — the per-CPU reclaim-available bandwidth. Read in-place from `cpu_rq(cpu)->dl.extra_bw` at check time.
- `max_bw` — the per-CPU maximum bandwidth (typically `(1 << BW_SHIFT) - 1`). Read in-place from `cpu_rq(cpu)->dl.max_bw` at check time.
- `total_bw` — the root domain's total allocated DL bandwidth. Read in-place from `dl_bw_of(cpu)->total_bw` under `dl_b->lock`.
- `nr_cpus` — the number of active CPUs in the root domain. Computed via `dl_bw_cpus(cpu)` (i.e., `cpumask_weight_and(rd->span, cpu_active_mask)`).

**Check(s):**

Check 1: Performed after `__dl_add()` / `__dl_sub()` returns (i.e., after any DL bandwidth modification such as `dl_server_apply_params()`, `__dl_server_attach_root()`, `__setparam_dl()`, or domain rebuild). Precondition: `nr_cpus > 0`.
```c
// For each active CPU in the root domain:
u64 expected_extra = max_bw - (total_bw / nr_cpus);
WARN_ON(cpu_rq(cpu)->dl.extra_bw != expected_extra);
```

Check 2: Performed at `scheduler_tick()` or any convenient periodic point, as a steady-state sanity check. Precondition: system is past SMP init (`sched_smp_initialized == true`) and `nr_cpus > 0`.
```c
struct dl_bw *dl_b = dl_bw_of(cpu);
int nr_cpus = dl_bw_cpus(cpu);
u64 expected_extra = cpu_rq(cpu)->dl.max_bw - (dl_b->total_bw / nr_cpus);

if (cpu_rq(cpu)->dl.extra_bw != expected_extra) {
    // Invariant violated: extra_bw is inconsistent with total_bw / nr_cpus
    WARN_ON_ONCE(1);
}
```

**Example violation:** The bug initializes dl_server bandwidth via `__dl_add()` when only 1 CPU is active (early boot), so `extra_bw` is reduced by `server_bw / 1` instead of `server_bw / N`. After all CPUs come online, each CPU's `extra_bw` is inconsistent with `total_bw / N` — the invariant fails on every CPU in the root domain.

**Other bugs caught:** `deadline_extra_bw_stale_rebuild` (stale extra_bw after root domain rebuild), `deadline_rd_bw_accounting_rebuild` (rebuild accounting mismatch) — both involve extra_bw diverging from the root domain's total_bw divided by CPU count.
