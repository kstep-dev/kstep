# DL Root Domain Bandwidth Capacity Invariant
**Source bug:** `a57415f5d1e43c3a5c5d412cd85e2792d7ed9b11`

**Property:** The total allocated SCHED_DEADLINE bandwidth in a root domain must not exceed the total available DL bandwidth across all CPUs in that root domain.

**Variables:**
- `total_bw` — total allocated DL bandwidth in the root domain. Read in-place from `dl_b->total_bw` under `dl_b->lock`.
- `bw` — per-CPU DL bandwidth limit. Read in-place from `dl_b->bw` (derived from `global_rt_runtime / global_rt_period`).
- `cpus` — number of active CPUs in the root domain. Obtained by calling `dl_bw_cpus(cpu)` which computes `cpumask_weight_and(rd->span, cpu_active_mask)`.

**Check(s):**

Check 1: Performed after `__dl_add()` / DL task admission (inside `dl_bw_of` lock). Whenever a DL task's bandwidth is added to `dl_b->total_bw`.
```c
// After adding task bandwidth to total_bw:
raw_spin_lock(&dl_b->lock);
// ... admission logic ...
WARN_ON_ONCE(dl_b->total_bw > dl_b->bw * cpus);
raw_spin_unlock(&dl_b->lock);
```

Check 2: Performed at `sched_dl_global_validate()`. When sysctl changes the global RT bandwidth parameters.
```c
raw_spin_lock_irqsave(&dl_b->lock, flags);
cpus = dl_bw_cpus(cpu);
// The new per-cpu bandwidth times the number of CPUs must accommodate total_bw
if (new_bw * cpus < dl_b->total_bw)
    ret = -EBUSY;
raw_spin_unlock_irqrestore(&dl_b->lock, flags);
```

Check 3: Performed after `rebuild_sched_domains` or cpuset changes that alter root domain membership. After CPUs are redistributed across root domains, each domain's total_bw must still fit within its new capacity.
```c
// After root domain rebuild, for each root domain:
raw_spin_lock(&dl_b->lock);
cpus = dl_bw_cpus(cpu);
WARN_ON_ONCE(dl_b->total_bw > dl_b->bw * cpus);
raw_spin_unlock(&dl_b->lock);
```

**Example violation:** The buggy `sched_dl_global_validate()` compared `new_bw` (per-CPU) directly against `dl_b->total_bw` (per-root-domain), failing to multiply by the number of CPUs. This caused valid bandwidth configurations to be rejected with `-EBUSY` on multi-CPU root domains.

**Other bugs caught:** Potentially `deadline_rd_bw_accounting_rebuild` and `deadline_extra_bw_stale_rebuild` (bandwidth accounting errors during root domain rebuilds could also violate this capacity invariant).
