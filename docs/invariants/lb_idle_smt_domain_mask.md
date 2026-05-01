# select_idle_sibling Must Return CPU Within Scheduling Domain
**Source bug:** `df3cb4ea1fb63ff326488efd671ba3c39034255e`

**Property:** The CPU returned by `select_idle_sibling()` must belong to the LLC scheduling domain span of the target CPU (or be the original target/prev).

**Variables:**
- `returned_cpu` — the CPU selected by `select_idle_sibling()`. Recorded at the return point of `select_idle_sibling()`. Read directly from the function's return value.
- `sd_llc_span` — the cpumask of the LLC scheduling domain for the target CPU. Recorded at `select_idle_sibling()` after `sd = rcu_dereference(per_cpu(sd_llc, target))`. Read via `sched_domain_span(sd)`.
- `target` — the initial target CPU passed to `select_idle_sibling()`. Read from function parameter.
- `prev` — the previous CPU the task ran on. Read from function parameter.

**Check(s):**

Check 1: Performed at the return of `select_idle_sibling()`, after the final CPU is chosen. Only when `sd != NULL` (i.e., the LLC domain exists and idle scanning was attempted).
```c
// At end of select_idle_sibling(), before returning i:
// sd was already looked up as: sd = rcu_dereference(per_cpu(sd_llc, target));
if (sd && i != target && i != prev) {
    // Any CPU found by the idle-scanning helpers must be in the domain
    WARN_ON_ONCE(!cpumask_test_cpu(i, sched_domain_span(sd)));
}
```

**Example violation:** `select_idle_smt()` iterates `cpu_smt_mask(target)` without checking `sched_domain_span(sd)`, so it returns an isolated CPU (e.g., CPU 17, an SMT sibling of non-isolated CPU 1) that is outside the scheduling domain. The check fires because the returned CPU is not in `sched_domain_span(sd)`.

**Other bugs caught:**
- `8aeaffef8c6eceab0e1498486fdd4f3dc3b7066c` — regression that removed the domain check from `select_idle_smt()` (same exact pattern)
- `23d04d8c6b8ec339057264659b7834027f3e6a63` — `select_idle_core()` checks `p->cpus_ptr` instead of domain-restricted mask, returning isolated CPUs
