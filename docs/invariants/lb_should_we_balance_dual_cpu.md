# Unique Load Balance Initiator Per Scheduling Group
**Source bug:** `6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3`

**Property:** For each scheduling group at a given domain level, `should_we_balance()` must return true for at most one CPU per periodic balance pass.

**Variables:**
- `selected_cpu` — the CPU for which `should_we_balance()` returned true, initiating `load_balance()`. Recorded at `sched_balance_rq` (or `load_balance` entry), once per balance pass per sched_domain. Read from `env->dst_cpu`.
- `sg_id` — identity of the scheduling group being balanced (e.g., first CPU in `sched_group->cpumask`). Recorded at `sched_balance_rq`. Read from `group_balance_cpu(sd->groups)`.
- `sd_level` — the scheduling domain level (SMT, MC, DIE, NUMA). Recorded at `sched_balance_rq`. Read from `sd->level`.
- `balance_generation` — a per-sched_group counter that tracks the current balance pass epoch. Stored in a shadow structure indexed by `(sg_id, sd_level)`. Incremented when the balance interval expires.
- `last_balance_cpu` — the CPU that was selected in the current balance pass for this `(sg_id, sd_level)`. Stored in the same shadow structure. Written on first selection, checked on subsequent ones.

**Check(s):**

Check 1: Performed at `load_balance()` entry (after `should_we_balance()` returns true). Precondition: `idle != CPU_NEWLY_IDLE` (only applies to periodic/tick-driven balancing).
```c
// Shadow structure per (sg_id, sd_level):
//   struct { int generation; int selected_cpu; } balance_tracker[NR_CPUS][MAX_SD_LEVELS];
//
// At load_balance() entry, after should_we_balance() returned true:
int sg = group_balance_cpu(sd->groups);
int level = sd->level;
int this_cpu = env->dst_cpu;

if (balance_tracker[sg][level].generation == current_balance_generation) {
    // Another CPU was already selected this pass — invariant violated
    WARN(balance_tracker[sg][level].selected_cpu != this_cpu,
         "Two CPUs selected for balance: cpu %d and cpu %d "
         "for sg %d at level %d",
         balance_tracker[sg][level].selected_cpu, this_cpu,
         sg, level);
} else {
    balance_tracker[sg][level].generation = current_balance_generation;
    balance_tracker[sg][level].selected_cpu = this_cpu;
}
```

**Example violation:** On a 4-CPU SMT system with topology `[0,1] [2,3]` where CPUs 0,1,3 are busy and CPU 2 is idle, `should_we_balance()` returns true for both CPU 0 (via `group_balance_cpu` fallback) and CPU 2 (via `idle_smt` match), causing two CPUs to enter `load_balance()` for the same scheduling group in the same balance pass.

**Other bugs caught:** Potentially any future logic error in the multi-tier selection within `should_we_balance()` that allows fall-through to a lower-priority path when a higher-priority path already identified a candidate.
