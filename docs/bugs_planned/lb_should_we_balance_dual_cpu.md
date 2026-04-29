# LB: should_we_balance() Allows Two CPUs to Load Balance Simultaneously

**Commit:** `6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.7-rc2
**Buggy since:** `b1bfeab9b002` ("sched/fair: Consider the idle state of the whole core for load balance"), merged in v6.6-rc1

## Bug Description

The `should_we_balance()` function in the CFS load balancer determines which CPU within a scheduling group should perform periodic load balancing. The design invariant is that exactly one CPU per scheduling group returns `true` from this function on each tick-driven load balance pass. This ensures that redundant load balancing work is avoided and that balancing decisions are coherent.

Commit `b1bfeab9b002` introduced logic to prefer CPUs on fully idle cores over idle SMT siblings with busy co-siblings when selecting the load-balancing CPU. The idea was that in hybrid x86 systems with a mix of SMT and non-SMT cores, an idle CPU on a fully idle core has more available capacity and should be preferred as the load-balancing initiator. When no fully idle core exists, the code falls back to the first idle SMT sibling recorded in the `idle_smt` variable.

However, the fallback logic in `b1bfeab9b002` contained a subtle bug: it only checked `if (idle_smt == env->dst_cpu)` and returned `true` in that case. When `idle_smt` did NOT match `dst_cpu`, the function fell through to the final fallback `return group_balance_cpu(sg) == env->dst_cpu`, which returns `true` for the first CPU in the group. This means that when no fully idle core exists, TWO CPUs can return `true`: the first idle SMT sibling (via the `idle_smt` check) and the first CPU in the group (via the `group_balance_cpu` fallback). This violates the design invariant that exactly one CPU should initiate load balancing.

## Root Cause

The root cause is a missing early return in the `idle_smt` fallback path of `should_we_balance()`. The buggy code is:

```c
static int should_we_balance(struct lb_env *env)
{
    struct sched_group *sg = env->sd->groups;
    int cpu, idle_smt = -1;
    ...
    for_each_cpu_and(cpu, swb_cpus, env->cpus) {
        if (!idle_cpu(cpu))
            continue;

        if (!(env->sd->flags & SD_SHARE_CPUCAPACITY) && !is_core_idle(cpu)) {
            if (idle_smt == -1)
                idle_smt = cpu;
            cpumask_andnot(swb_cpus, swb_cpus, cpu_smt_mask(cpu));
            continue;
        }

        /* Are we the first idle CPU? */
        return cpu == env->dst_cpu;
    }

    /* BUG: Only returns true when idle_smt matches, falls through otherwise */
    if (idle_smt == env->dst_cpu)
        return true;

    /* Fallback: first CPU in the group */
    return group_balance_cpu(sg) == env->dst_cpu;
}
```

Consider a 4-CPU system with two SMT cores: `[0, 1] [2, 3]`, where `b` = busy and `i` = idle:

```
[0, 1] [2, 3]
 b  b   i  b
```

Core 0 (CPUs 0, 1) is fully busy. Core 1 (CPUs 2, 3) is partially busy — CPU 2 is idle, CPU 3 is busy.

When `should_we_balance()` is called at the MC (multi-core) domain level, the sched domain does NOT have `SD_SHARE_CPUCAPACITY` set (that flag is only on the SMT-level domain). The function iterates over CPUs in the group balance mask in order:

**For CPU 0 as `dst_cpu`:**
1. CPU 0: `idle_cpu(0)` → false (busy) → skip
2. CPU 1: `idle_cpu(1)` → false (busy) → skip
3. CPU 2: `idle_cpu(2)` → true. Check: `!(SD_SHARE_CPUCAPACITY)` is true (MC domain), `!is_core_idle(2)` is true (CPU 3 is busy). So `idle_smt = 2`, and CPUs 2, 3 are removed from the mask via `cpumask_andnot`. Continue.
4. CPU 3: already removed from mask, not iterated.
5. Loop ends — no fully idle core was found.
6. `idle_smt == env->dst_cpu` → `2 == 0` → false. Falls through.
7. `group_balance_cpu(sg) == env->dst_cpu` → `0 == 0` → **true** (CPU 0 is the first CPU in the group).

**For CPU 2 as `dst_cpu`:**
1. CPU 0: not idle → skip
2. CPU 1: not idle → skip
3. CPU 2: idle, but core not fully idle → `idle_smt = 2`, skip core 1
4. Loop ends.
5. `idle_smt == env->dst_cpu` → `2 == 2` → **true**

Both CPU 0 and CPU 2 return `true`. CPU 0 is selected via the `group_balance_cpu` fallback because the `idle_smt` check did not prevent the fall-through. CPU 2 is selected because it matches `idle_smt`. This is the core logic error: when `idle_smt != -1`, the function should have returned `idle_smt == env->dst_cpu` unconditionally, regardless of whether it matches or not, to prevent the `group_balance_cpu` fallback from also returning `true`.

## Consequence

The immediate consequence is that two CPUs within the same scheduling group both perform periodic load balancing when only one should. This has several negative effects:

1. **Redundant work under the runqueue lock**: Both CPU 0 and CPU 2 independently call `find_busiest_group()`, `find_busiest_queue()`, and potentially `detach_tasks()` while holding runqueue locks. This doubles the load balancing overhead and increases lock contention. On systems with many cores and frequent tick-driven balancing, this can measurably degrade throughput.

2. **Conflicting migration decisions**: Two independent load balancers examining the same scheduling group may make conflicting migration decisions. One CPU may attempt to pull tasks that the other is also trying to pull, leading to unnecessary lock contention on the busiest runqueue and potentially suboptimal task placement.

3. **Incorrect balancing initiator**: CPU 0 in the example is fully busy — it has no idle capacity. Having a busy CPU initiate load balancing is counterproductive because the purpose of periodic load balancing is to move work TO idle CPUs. CPU 0 would be pulling tasks from other busy CPUs to itself (an already busy CPU), rather than letting idle CPU 2 pull tasks to where they can run immediately. This can cause unnecessary task migrations and increased cache-miss overhead.

The bug does not cause a kernel crash or hang, but it causes measurable scheduling inefficiency on SMT systems where not all cores are fully idle. The impact scales with the number of SMT cores and the frequency of tick-driven load balancing. Workloads that leave some SMT siblings idle while others are busy (a very common scenario) are affected.

## Fix Summary

The fix changes the `idle_smt` fallback from a conditional early return to an unconditional early return when any idle SMT sibling with busy co-siblings was found:

```c
/* Before (buggy): */
if (idle_smt == env->dst_cpu)
    return true;

/* After (fixed): */
if (idle_smt != -1)
    return idle_smt == env->dst_cpu;
```

The key insight is that when `idle_smt != -1`, the loop has completed without finding a fully idle core, but it DID find at least one idle CPU on a partially-busy core. In this case, the load balancing should be performed by that first idle SMT sibling (`idle_smt`) and ONLY that CPU. The function must return a definitive answer (`idle_smt == env->dst_cpu`) and must NOT fall through to the `group_balance_cpu` fallback.

The fix also updates the comment from "Are we the first idle CPU?" to the more precise "Are we the first idle core in a non-SMT domain or higher, or the first idle CPU in a SMT domain?" for the in-loop return, and adds the comment "Are we the first idle CPU with busy siblings?" for the `idle_smt` check. These comments clarify the three-tier priority of CPU selection in `should_we_balance()`:
1. First priority: a CPU on a fully idle core (returned in the loop body).
2. Second priority: the first idle SMT sibling with busy co-siblings (`idle_smt`).
3. Third priority (last resort): the `group_balance_cpu` — the first CPU in the group, regardless of idle state.

With the fix, the `group_balance_cpu` fallback is only reached when there are NO idle CPUs at all (neither on idle cores nor as idle SMT siblings), ensuring that exactly one CPU is selected in all cases.

## Triggering Conditions

The bug requires the following precise conditions:

- **SMT topology**: The system must have at least two SMT cores (4+ CPUs with hyperthreading). The bug occurs at the MC (multi-core) or higher scheduling domain level, where `SD_SHARE_CPUCAPACITY` is NOT set. The SMT-level domain (where `SD_SHARE_CPUCAPACITY` IS set) is not affected because the `!is_core_idle` path is skipped at that level.

- **No fully idle core**: Every SMT core must have at least one busy sibling. If any core is fully idle (all siblings idle), the loop returns early on the first idle CPU of that core, and the `idle_smt` path is never reached.

- **At least one idle SMT sibling**: At least one core must be partially busy (one sibling idle, one busy). The idle sibling becomes the `idle_smt` candidate.

- **The first CPU in the group must not be the idle_smt CPU**: The `group_balance_cpu(sg)` must return a different CPU than `idle_smt`. Since `group_balance_cpu` returns the first CPU in the group (typically the lowest-numbered), and `idle_smt` is the first idle CPU found in iteration order, this happens when a lower-numbered core is fully busy. In the example, core 0 (CPUs 0, 1) is all busy, so `group_balance_cpu` returns 0, while `idle_smt` = 2.

- **Tick-driven load balancing**: The bug is only triggered during periodic (tick-driven) load balancing, not during `CPU_NEWLY_IDLE` balancing (which has its own early-return path).

- **CONFIG_SCHED_SMT enabled**: Required for SMT-aware scheduling. Without it, `is_core_idle()` is not available and the `idle_smt` logic is not compiled in.

The minimal topology to trigger the bug is: 2 SMT cores (4 CPUs), with one core fully busy and the other partially busy (one idle, one busy). This is extremely common in real workloads — any situation where 3 out of 4 CPUs are busy on a dual-core hyperthreaded system will trigger it.

## Reproduce Strategy (kSTEP)

### Overview

This bug is reproducible with kSTEP. The key is to set up an SMT topology, create a workload pattern where no core is fully idle but one SMT sibling is idle, then observe that both the first CPU in the group AND the idle SMT sibling attempt load balancing on the buggy kernel.

### Step-by-step Plan

**1. QEMU Configuration:**
Configure QEMU with at least 4 CPUs (remember CPU 0 is reserved for the driver, so we need CPUs 1-4 for the workload, meaning 5 CPUs total). However, since CPU 0 is reserved, we should use CPUs 1-4 for the SMT topology. Set up with 5 CPUs.

Actually, since CPU 0 is reserved, we should design the topology around CPUs 1-4 as two SMT cores: `[1,2] [3,4]`. This way CPU 0 (the driver CPU) is separate.

**2. Topology Setup:**
```c
kstep_topo_init();
const char *smt[] = {"1,2", "3,4"};
kstep_topo_set_smt(smt, 2);
const char *mc[] = {"1-4"};
kstep_topo_set_mc(mc, 1);
const char *pkg[] = {"0-4"};
kstep_topo_set_pkg(pkg, 1);
kstep_topo_apply();
```

**3. Task Creation and Pinning:**
Create 3 CFS tasks and pin them to establish the pattern `[b, b] [i, b]` on cores `[1,2] [3,4]`:
- Task A: pinned to CPU 1 (busy)
- Task B: pinned to CPU 2 (busy)
- Task C: pinned to CPU 4 (busy)
- CPU 3 remains idle

```c
struct task_struct *tA = kstep_task_create();
kstep_task_pin(tA, 1, 1);
struct task_struct *tB = kstep_task_create();
kstep_task_pin(tB, 2, 2);
struct task_struct *tC = kstep_task_create();
kstep_task_pin(tC, 4, 4);
```

**4. Triggering Load Balance:**
Use `kstep_tick_repeat()` to advance ticks. The periodic load balancer runs from the scheduler tick via `trigger_load_balance()` → SCHED_SOFTIRQ → `rebalance_domains()` → `load_balance()` → `should_we_balance()`. After several ticks, the load balancer will be invoked on each CPU.

**5. Detection via `on_sched_balance_selected` Callback:**
The `on_sched_balance_selected` callback fires after `should_we_balance()` returns `true` — it is called when a CPU is selected to actually perform load balancing for a given sched domain. Track which CPUs trigger this callback at the MC domain level:

```c
static int balance_selected_cpu1 = 0;
static int balance_selected_cpu3 = 0;

void on_balance_selected(int cpu, struct sched_domain *sd) {
    if (!(sd->flags & SD_SHARE_CPUCAPACITY)) {  /* MC domain, not SMT */
        if (cpu == 1) balance_selected_cpu1++;
        if (cpu == 3) balance_selected_cpu3++;
        pr_info("balance_selected: cpu=%d domain_flags=0x%x\n", cpu, sd->flags);
    }
}
```

**6. Pass/Fail Criteria:**
- On the **buggy kernel**: Both CPU 1 (the first CPU in the group, which is busy) and CPU 3 (the idle SMT sibling) will have `on_sched_balance_selected` fire at the MC domain. `balance_selected_cpu1 > 0` AND `balance_selected_cpu3 > 0`.
- On the **fixed kernel**: Only CPU 3 (the idle SMT sibling = `idle_smt`) will be selected. `balance_selected_cpu1 == 0` AND `balance_selected_cpu3 > 0`.

The driver should call `kstep_fail()` if `balance_selected_cpu1 > 0` on the buggy kernel test (indicating the bug is present — which is what we want to observe), and `kstep_pass()` if only CPU 3 is selected on the fixed kernel.

More precisely, for a "reproduction" driver:
- **Buggy kernel**: `kstep_fail("CPU 1 (busy) incorrectly selected for balance: %d times", balance_selected_cpu1)` when `balance_selected_cpu1 > 0` — this CONFIRMS the bug is triggered.
- **Fixed kernel**: `kstep_pass("Only idle SMT CPU 3 selected for balance")` when `balance_selected_cpu1 == 0`.

**7. Alternative Detection via schedstat:**
If `on_sched_balance_selected` does not fire with sufficient granularity, an alternative approach is to use `KSYM_IMPORT` to access the per-CPU `struct sched_domain` and read `sd->lb_count[CPU_IDLE]` and `sd->lb_balanced[CPU_IDLE]` schedstat counters before and after tick cycles. On the buggy kernel, the MC-level domain for CPU 1 would show elevated `lb_count` (indicating it entered `load_balance()` and passed `should_we_balance()`), while on the fixed kernel it would show only `lb_balanced` increments (indicating `should_we_balance()` returned false and the function exited via `out_balanced`).

**8. Expected Tick Count:**
The periodic load balance interval depends on the sched domain's `balance_interval` (typically a few hundred milliseconds to seconds, scaled by `sd_weight`). With 4 CPUs and default settings, the MC domain balance interval is approximately 4ms. Run `kstep_tick_repeat(100)` (100 ticks at default 4ms interval = 400ms) to ensure multiple balance passes occur. Adjust based on actual domain configuration.

**9. Robustness:**
The bug is fully deterministic given the correct topology and workload pattern. There is no race condition or timing sensitivity — the issue is a pure logic error in `should_we_balance()` that produces the wrong result every time the specified conditions are met. The driver should produce consistent results across runs.

**10. kSTEP Extensions Needed:**
No extensions to kSTEP are needed. The existing `kstep_topo_set_smt()`, `kstep_task_pin()`, `kstep_tick_repeat()`, and `on_sched_balance_selected` callback provide all required functionality. `KSYM_IMPORT` can be used to access `struct sched_domain` internals if schedstat-based detection is preferred.
