# Core Scheduling SMT Sibling Core Pointer Consistency
**Source bug:** `3c474b3239f12fe0b00d7e82481f36a1f31e79ab`

**Property:** For every possible CPU, `rq->core` must be a valid non-NULL pointer to a `struct rq` belonging to the same SMT group (or itself), and all online SMT siblings must agree on the same core leader.

**Variables:**
- `rq->core` — pointer to the core leader's rq for this CPU. Read in-place from `struct rq`. Relevant at any point where `rq_lockp()` may be called.
- `cpu_smt_mask(cpu)` — the set of SMT siblings for a given CPU. Read via topology helpers.

**Check(s):**

Check 1: Performed at `rq_lockp()` / `raw_spin_rq_lock_nested()`. Whenever core scheduling is enabled and any code acquires an rq lock.
```c
// rq->core must never be NULL
if (sched_core_enabled(rq))
    WARN_ON_ONCE(rq->core == NULL);
```

Check 2: Performed at `sched_core_cpu_starting()` exit and `sched_core_cpu_deactivate()` exit. After any topology change completes (under sched_core_lock).
```c
// All online SMT siblings must point to the same core leader
const struct cpumask *smt_mask = cpu_smt_mask(cpu);
struct rq *expected_core = NULL;
int t;
for_each_cpu(t, smt_mask) {
    struct rq *rq = cpu_rq(t);
    if (!expected_core)
        expected_core = rq->core;
    WARN_ON_ONCE(rq->core != expected_core);
}
// The core leader must be one of the siblings
WARN_ON_ONCE(!cpumask_test_cpu(expected_core->cpu, smt_mask));
```

Check 3: Performed at `sched_core_cpu_dying()` exit. After a CPU has fully gone offline.
```c
// Dying CPU must be reset to self-referencing
struct rq *rq = cpu_rq(cpu);
WARN_ON_ONCE(rq->core != rq);
```

**Example violation:** Before the fix, `sched_init()` set `rq->core = NULL` for all CPUs, and only `sched_core_cpu_starting()` assigned a valid pointer on CPU online. CPUs that never came online retained `rq->core == NULL`, so any `for_each_possible_cpu()` loop that called `rq_lockp()` with core scheduling enabled hit a NULL dereference — violating Check 1.

**Other bugs caught:** Potentially catches bugs in CPU hotplug paths where `rq->core` is left stale or inconsistent after topology changes (e.g., missing `sched_core_cpu_deactivate`/`sched_core_cpu_dying` handling).
