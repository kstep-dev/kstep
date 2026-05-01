# Idle CPU Selection Must Respect Scheduling Domain Span
**Source bug:** `23d04d8c6b8ec339057264659b7834027f3e6a63`

**Property:** Any CPU returned by `select_idle_sibling()` (and its helpers `select_idle_core()`, `select_idle_smt()`, `select_idle_cpu()`) must belong to the scheduling domain span of the relevant domain, i.e., it must be in `sched_domain_span(sd)`.

**Variables:**
- `selected_cpu` — the CPU returned by `select_idle_sibling()`. Recorded at the return point of `select_idle_sibling()`. Read directly from the return value.
- `sd` — the scheduling domain used for the idle search (the LLC-level domain, `sd_llc`). Recorded at entry to `select_idle_sibling()` / `select_idle_cpu()`. Read via `rcu_dereference(per_cpu(sd_llc, target))`.
- `sd_span` — the cpumask of CPUs in the scheduling domain. Derived from `sched_domain_span(sd)`.

**Check(s):**

Check 1: Performed at the return of `select_idle_sibling()`. Only when the return value is a valid CPU (>= 0) and an LLC scheduling domain exists.
```c
// At the end of select_idle_sibling(), before returning new_cpu:
struct sched_domain *sd = rcu_dereference(per_cpu(sd_llc, target));
if (new_cpu >= 0 && sd) {
    WARN_ON_ONCE(!cpumask_test_cpu(new_cpu, sched_domain_span(sd)) &&
                 !cpumask_test_cpu(new_cpu, p->cpus_ptr));
    // The stronger check: selected CPU must be in domain span
    // (cpus_ptr alone is insufficient when isolcpus is active)
    WARN_ON_ONCE(!cpumask_test_cpu(new_cpu, sched_domain_span(sd)));
}
```

Check 2: Performed inside `select_idle_core()` when setting `*idle_cpu`. Each time a candidate is recorded.
```c
// When select_idle_core() sets *idle_cpu = cpu:
// The cpu must be in the 'cpus' mask (which is sd_span & p->cpus_ptr)
if (*idle_cpu == -1 && cpumask_test_cpu(cpu, cpus)) {
    *idle_cpu = cpu;
    // invariant: cpumask_test_cpu(cpu, cpus) implies
    // cpumask_test_cpu(cpu, sched_domain_span(sd))
}
```

**Example violation:** With `isolcpus=domain,3` on an SMT system (cores {2,3}), `select_idle_core()` iterates SMT siblings of core 2 and finds CPU 3 idle. It checks `cpumask_test_cpu(3, p->cpus_ptr)` which is true (default affinity includes all online CPUs), so it sets `*idle_cpu = 3`. But CPU 3 is not in `sched_domain_span(sd)`, violating the invariant. The fix changes the check to use the `cpus` mask (which is `sd_span & cpus_ptr`), correctly excluding CPU 3.

**Other bugs caught:**
- `8aeaffef8c6eceab0e1498486fdd4f3dc3b7066c` — `select_idle_smt()` same class: checks `p->cpus_ptr` instead of domain-restricted mask
- `df3cb4ea1fb63ff326488efd671ba3c39034255e` — `select_idle_smt()` original instance: no domain span check at all
