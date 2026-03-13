# LB: Redundant SMT sibling iteration in should_we_balance()

**Commit:** `f8858d96061f5942216c6abb0194c3ea7b78e1e8`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.6-rc2
**Buggy since:** v6.6-rc1 (introduced by `b1bfeab9b002` "sched/fair: Consider the idle state of the whole core for load balance")

## Bug Description

The `should_we_balance()` function in the CFS load balancer determines which CPU within a scheduling group should be the one to perform load balancing. After commit `b1bfeab9b002`, this function was enhanced to prefer CPUs on fully idle cores over idle SMT siblings on partially busy cores. The rationale was that on hybrid x86 systems (e.g., with both SMT and non-SMT cores), an idle CPU on a fully idle core has more available capacity than an idle SMT sibling whose core-mate is busy, so it should get priority for idle load balancing.

However, the implementation introduced a performance regression on systems with high SMT counts (e.g., POWER10 with SMT8, yielding 96 CPUs across 12 cores). When iterating through the `group_balance_mask` to find the first idle CPU, the code calls `is_core_idle(cpu)` for every idle CPU it encounters. If a core is not idle (i.e., at least one sibling is busy), the code records the first idle SMT sibling in `idle_smt` and continues to the next CPU. Crucially, it does NOT skip the remaining SMT siblings of that same non-idle core. This means every remaining idle sibling of the same non-idle core will be individually checked: each triggers another `is_core_idle()` call that iterates through all siblings — producing O(SMT²) work per non-idle core.

On POWER10 with SMT8, if one CPU in an 8-way SMT core is busy and the other 7 are idle, the buggy code checks `is_core_idle()` 7 times (once for each idle sibling), and each `is_core_idle()` call iterates over all 8 siblings. That is 56 iterations for a single core where only 1 check (on the first idle sibling) would suffice. Across all cores in the DIE/NUMA domain, this overhead multiplies, causing a measurable performance regression of 5–16% in the time spent in `should_we_balance()`.

## Root Cause

The root cause is the lack of early termination when iterating over SMT siblings of a non-idle core in `should_we_balance()`. The relevant buggy code path (after `b1bfeab9b002`) is:

```c
for_each_cpu_and(cpu, group_balance_mask(sg), env->cpus) {
    if (!idle_cpu(cpu))
        continue;

    if (!(env->sd->flags & SD_SHARE_CPUCAPACITY) && !is_core_idle(cpu)) {
        if (idle_smt == -1)
            idle_smt = cpu;
        continue;   // <-- continues to next CPU, which may be another sibling of the SAME core
    }

    return cpu == env->dst_cpu;
}
```

The function `is_core_idle(cpu)` is defined as:

```c
static inline bool is_core_idle(int cpu) {
    int sibling;
    for_each_cpu(sibling, cpu_smt_mask(cpu)) {
        if (cpu == sibling) continue;
        if (!idle_cpu(sibling)) return false;
    }
    return true;
}
```

When the scheduler domain does NOT have `SD_SHARE_CPUCAPACITY` (i.e., the balancing domain is MC/DIE/NUMA, not the SMT domain itself), and a core is not fully idle, the code correctly records `idle_smt` for the first idle sibling it finds. But it then `continue`s to the next CPU in the mask. Since `group_balance_mask` includes all CPUs in the group, the next CPU may well be another sibling of the exact same non-idle core. For that sibling, `idle_cpu()` returns true, then `is_core_idle()` is called again — iterating through all siblings again — only to discover the core is still not idle. The `idle_smt` variable is already set, so it doesn't update. This is pure wasted work.

On an SMT-N system, for each non-idle core with K idle siblings (K ≤ N-1), the buggy code performs K calls to `is_core_idle()`, each iterating over N siblings, yielding K×N iterations. With the fix, only 1 call to `is_core_idle()` is made per non-idle core, after which all remaining siblings are removed from the iteration mask.

## Consequence

The consequence is a measurable performance degradation in the load balancing hot path. On POWER10 systems with SMT8 (96 CPUs across 12 cores), benchmarks showed that `should_we_balance()` took 5–16% longer than necessary, depending on workload:

- Idle system: 809 ns → 695 ns with fix (16.5% improvement)
- 12 stress-ng threads at 100% utilization: 1013 ns → 893 ns (13.5% improvement)
- 24 stress-ng threads at 100% utilization: 1073 ns → 980 ns (9.5% improvement)

This overhead directly impacts scheduling latency. Since `should_we_balance()` is called on every regular load balance tick by every CPU (via `rebalance_domains()` → `load_balance()`), the cumulative overhead is significant. It manifests as higher tail latencies in workloads sensitive to scheduling responsiveness — schbench showed improvements of up to 36% at the 99.9th percentile with the fix applied.

Importantly, this is purely a performance regression. The function returns the correct result (same boolean true/false) on both buggy and fixed kernels. No incorrect scheduling decisions are made; no crashes or data corruption occur. The only impact is wasted CPU cycles in the scheduler's load balancing path, which translates to higher scheduling latency and reduced throughput, especially on high-SMT-count architectures.

## Fix Summary

The fix introduces a per-CPU temporary cpumask `should_we_balance_tmpmask` that is used as a mutable copy of `group_balance_mask(sg)`. When iterating through CPUs and encountering the first idle sibling of a non-idle core, after recording it in `idle_smt`, the fix removes all remaining siblings of that core from the temporary mask using `cpumask_andnot(swb_cpus, swb_cpus, cpu_smt_mask(cpu))`. This ensures the loop immediately skips over the remaining siblings of that core.

Specifically, the fix makes three changes:
1. Adds a new per-CPU cpumask variable `should_we_balance_tmpmask`, allocated at init time in `init_sched_fair_class()`.
2. At the start of the non-NEWLY_IDLE path in `should_we_balance()`, copies `group_balance_mask(sg)` into the local `swb_cpus` mask.
3. After finding an idle CPU in a non-idle core (the `!is_core_idle(cpu)` branch), calls `cpumask_andnot(swb_cpus, swb_cpus, cpu_smt_mask(cpu))` under `#ifdef CONFIG_SCHED_SMT` to remove all siblings of that core from future iteration.

The fix is correct because: (a) it only removes siblings of cores that are already determined to be non-idle, so no CPU on a fully idle core is ever skipped; (b) the first idle sibling found in each non-idle core is still correctly recorded in `idle_smt`; (c) the copy-then-modify approach avoids corrupting the shared `group_balance_mask`.

## Triggering Conditions

The performance regression requires:

1. **CONFIG_SMP=y and CONFIG_SCHED_SMT=y**: The bug is in the SMT-aware path of `should_we_balance()`, guarded by `!(env->sd->flags & SD_SHARE_CPUCAPACITY)` and the `is_core_idle()` function which is only compiled with `CONFIG_SCHED_SMT`.

2. **High SMT count (SMT ≥ 4)**: The overhead scales quadratically with SMT level. On SMT2, the waste is minimal (at most 1 extra `is_core_idle()` call per core). On SMT4, up to 3 redundant calls per core. On SMT8 (POWER10), up to 7 redundant calls per core, each iterating 8 siblings.

3. **Partially busy cores**: The regression only occurs when cores have a mix of busy and idle siblings. If all cores are fully idle (all siblings idle), `is_core_idle()` returns true and the loop terminates immediately at the first idle CPU. If all cores are fully busy, `idle_cpu()` returns false and the `is_core_idle()` path is never reached. The worst case is when exactly one sibling per core is busy and the rest are idle.

4. **Load balancing at MC/DIE/NUMA domain level**: The `SD_SHARE_CPUCAPACITY` check means this code path is only active when balancing at a domain that does NOT share CPU capacity (i.e., above the SMT domain level). This is the MC (multi-core), DIE, or NUMA scheduling domain.

5. **Non-NEWLY_IDLE balancing**: The `should_we_balance()` code has an early return for `CPU_NEWLY_IDLE` that bypasses the problematic loop entirely. The regression only manifests during regular periodic or idle load balancing.

The regression is deterministic and always present when the above conditions hold. It is not a race condition — it is a straightforward algorithmic inefficiency that occurs on every invocation of `should_we_balance()` in the affected configuration.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**

   This is a **pure performance regression**, not a correctness bug. The buggy and fixed kernels produce **identical scheduling decisions** — `should_we_balance()` returns the exact same boolean result on both kernels for any given input state. The only difference is the CPU time spent computing that result: the buggy kernel iterates redundantly over SMT siblings of non-idle cores, while the fixed kernel skips them.

   kSTEP's observation APIs (`kstep_pass`, `kstep_fail`, `kstep_output_curr_task`, `kstep_output_balance`, etc.) are designed to detect differences in scheduling behavior — which task runs on which CPU, when context switches occur, load balance decisions. None of these can distinguish between the buggy and fixed kernels because the scheduling outcomes are identical.

   To detect this bug, one would need to measure either: (a) the wall-clock execution time of `should_we_balance()`, which is infeasible in QEMU because TCG emulation introduces orders-of-magnitude timing noise that dwarfs the nanosecond-level difference; or (b) the number of iterations inside the `for_each_cpu_and` loop or the number of calls to `is_core_idle()`, which are internal implementation details not exposed through any kernel API or kSTEP observation mechanism.

2. **WHAT would need to be added to kSTEP to support this?**

   To reproduce this performance regression in kSTEP, fundamental instrumentation capabilities would be needed:

   - **Function call counting**: A mechanism to count how many times `is_core_idle()` is called during each invocation of `should_we_balance()`. This could be implemented via kprobes (registering a probe on `is_core_idle` and incrementing a per-CPU counter), but `is_core_idle` is a `static inline` function that is likely inlined by the compiler, making it impossible to kprobe.

   - **Kernel source modification**: Alternatively, a counter variable could be added directly to the kernel source code inside the `for_each_cpu_and` loop in `should_we_balance()`, read out by the kSTEP driver after each load balance invocation. This requires modifying the kernel source for each build, which is outside kSTEP's kernel-module-based architecture.

   - **Reliable timing measurement**: If timing were used instead, kSTEP would need a way to measure function execution time with nanosecond precision inside QEMU, which is not feasible because QEMU's TCG emulation does not provide cycle-accurate timing.

   None of these are minor extensions — they require either modifying the kernel source, adding kprobe infrastructure that doesn't work on inlined functions, or overcoming fundamental limitations of QEMU's timing model.

3. **Alternative reproduction methods outside kSTEP:**

   The original developer used the `funclatency` BCC/eBPF tool to measure the execution time of `should_we_balance()` (after making it `noinline`) on real POWER10 hardware with SMT8. This approach works well:

   - Boot a v6.6-rc1 kernel on a system with high SMT count (SMT4 or SMT8).
   - Make `should_we_balance()` noinline (add `__attribute__((noinline))` or `noinline` annotation).
   - Run `funclatency -d 60 should_we_balance` under various workloads (idle, stress-ng with partial CPU utilization).
   - Compare average latency with and without the fix applied.

   Alternatively, on any system with SMT ≥ 4, one could add a `printk`-based iteration counter inside the loop and compare total iterations between buggy and fixed kernels under the same workload. This would show the O(SMT²) vs O(SMT) difference directly.

   On real hardware with performance counters, one could also use `perf stat` to measure instruction counts or cycles spent in `should_we_balance()` to confirm the overhead reduction.
