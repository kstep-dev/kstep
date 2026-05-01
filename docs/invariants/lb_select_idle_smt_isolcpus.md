# Idle CPU Selection Must Respect Scheduling Domain Span
**Source bug:** `8aeaffef8c6eceab0e1498486fdd4f3dc3b7066c`

**Property:** Any CPU chosen by `select_idle_sibling()` (and its helpers `select_idle_smt()`, `select_idle_core()`, `select_idle_cpu()`) must belong to `sched_domain_span(sd)` of the LLC scheduling domain used for the search.

**Variables:**
- `selected_cpu` — the CPU returned by `select_idle_sibling()`. Recorded at the return point of `select_idle_sibling()`. Read directly from the return value.
- `sd` — the LLC scheduling domain for the target CPU. Recorded at `select_idle_sibling()` via `rcu_dereference(per_cpu(sd_llc, target))`. Read in-place.
- `target` — the initial target CPU passed into `select_idle_sibling()`. Read from the function parameter.

**Check(s):**

Check 1: Performed at the return point of `select_idle_sibling()`. Precondition: `sd != NULL` and the returned CPU differs from `prev` and `target` (i.e., the function actually searched for a new idle CPU rather than returning an early-exit candidate).
```c
// At the end of select_idle_sibling(), before returning i:
// If sd is valid and the selected CPU came from one of the idle-search
// helpers (not the early-exit paths for prev/target/recent_used_cpu),
// it must be in the scheduling domain.
if (sd && i != target && i != prev && (unsigned int)i < nr_cpumask_bits) {
    WARN_ON_ONCE(!cpumask_test_cpu(i, sched_domain_span(sd)));
}
```

**Example violation:** With `isolcpus=domain,3` on an SMT system, `select_idle_smt()` iterates hardware SMT siblings of the target CPU and returns CPU 3 (isolated, idle) because it never checks `sched_domain_span(sd)`. The returned CPU violates the invariant since CPU 3 is not in the domain span.

**Other bugs caught:**
- `df3cb4ea1fb63ff326488efd671ba3c39034255e` — original `select_idle_smt()` missing domain check (v5.10 fix)
- `23d04d8c6b8ec339057264659b7834027f3e6a63` — `select_idle_core()` using `p->cpus_ptr` instead of domain-restricted mask (v6.9 fix)
