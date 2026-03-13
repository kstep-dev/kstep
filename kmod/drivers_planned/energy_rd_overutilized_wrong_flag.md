# Energy: rd->sg_overutilized set from wrong variable (sg_overloaded)

**Commit:** `cd18bec668bb6221a54f03d0b645b7aed841f825`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.10-rc1
**Buggy since:** v6.10-rc1 (introduced by `4475cd8bfd9b` "sched/balancing: Simplify the sg_status bitmask and use separate ->overloaded and ->overutilized flags", merged in the same release cycle)

## Bug Description

In the load balancing function `update_sd_lb_stats()`, which runs periodically on the root scheduling domain to update scheduler statistics, a copy-paste error causes the root domain's `overutilized` flag (`rd->overutilized`) to be set from the wrong variable. The `sg_overloaded` boolean (which tracks whether any CPU has more than one runnable task) is used instead of the `sg_overutilized` boolean (which tracks whether any CPU's utilization exceeds its capacity). These are semantically distinct conditions: a system can be overutilized without being overloaded (one heavy task saturating a small CPU) or overloaded without being overutilized (several light tasks coexisting on a large CPU).

The `rd->overutilized` flag is the gating mechanism for Energy Aware Scheduling (EAS). When `rd->overutilized` is `false`, the scheduler uses `find_energy_efficient_cpu()` to select the most energy-efficient CPU for waking tasks. When `rd->overutilized` is `true`, the scheduler falls back to standard load-balancing heuristics that don't consider energy efficiency. By setting `rd->overutilized` to the value of `sg_overloaded` instead of `sg_overutilized`, the EAS on/off decision is based on the wrong metric.

This bug was introduced by commit `4475cd8bfd9b` which refactored the `sg_status` bitmask (which previously combined `SG_OVERLOADED` and `SG_OVERUTILIZED` flags into a single integer) into two separate boolean variables: `sg_overloaded` and `sg_overutilized`. During this refactoring, one of the two call sites that previously used `sg_status & SG_OVERUTILIZED` was incorrectly changed to reference `sg_overloaded` instead of `sg_overutilized`. The error was caught during code review by Vincent Guittot and fixed before the v6.10 release.

## Root Cause

The bug resides in `update_sd_lb_stats()` in `kernel/sched/fair.c`. Before the refactoring commit `4475cd8bfd9b`, the code used a single bitmask variable `sg_status` with flag constants:

```c
int sg_status = 0;
// ... in the loop:
*sg_status |= SG_OVERLOADED;    // if nr_running > 1
*sg_status |= SG_OVERUTILIZED;  // if cpu_overutilized(i)
// ... after the loop:
if (!env->sd->parent) {
    set_rd_overloaded(env->dst_rq->rd, sg_status & SG_OVERLOADED);
    set_rd_overutilized_status(env->dst_rq->rd, sg_status & SG_OVERUTILIZED);
} else if (sg_status & SG_OVERUTILIZED) {
    set_rd_overutilized_status(env->dst_rq->rd, SG_OVERUTILIZED);
}
```

Commit `4475cd8bfd9b` (by Ingo Molnar) split this into two separate booleans:

```c
bool sg_overloaded = 0, sg_overutilized = 0;
```

The `update_sg_lb_stats()` callee was correctly updated to use `bool *sg_overloaded` and `bool *sg_overutilized` pointers. However, in the caller's root-domain update block, the second `set_rd_overutilized()` call was incorrectly changed to:

```c
if (!env->sd->parent) {
    set_rd_overloaded(env->dst_rq->rd, sg_overloaded);
    set_rd_overutilized(env->dst_rq->rd, sg_overloaded);  // BUG: should be sg_overutilized
} else if (sg_overutilized) {
    set_rd_overutilized(env->dst_rq->rd, sg_overutilized);
}
```

Note that the `else if` branch correctly uses `sg_overutilized` — only the root-domain path (`!env->sd->parent`) has the error. This means the bug specifically affects systems where the load balancer reaches the root scheduling domain, which is the common case for systems with a single scheduling domain hierarchy.

The function `set_rd_overutilized()` writes to `rd->overutilized` (only when `sched_energy_enabled()` returns true):

```c
static inline void set_rd_overutilized(struct root_domain *rd, bool flag)
{
    if (!sched_energy_enabled())
        return;
    WRITE_ONCE(rd->overutilized, flag);
    trace_sched_overutilized_tp(rd, flag);
}
```

And `rd->overutilized` is read by `is_rd_overutilized()`:

```c
static inline bool is_rd_overutilized(struct root_domain *rd)
{
    return !sched_energy_enabled() || READ_ONCE(rd->overutilized);
}
```

This is consumed in `select_task_rq_fair()` to gate the EAS path:

```c
if (!is_rd_overutilized(this_rq()->rd)) {
    new_cpu = find_energy_efficient_cpu(p, prev_cpu);
    if (new_cpu >= 0)
        return new_cpu;
    new_cpu = prev_cpu;
}
```

Thus the entire EAS task placement decision at wake-up time is gated by the value of `rd->overutilized`, which is now set incorrectly based on `sg_overloaded`.

## Consequence

The impact is twofold, depending on which way the `sg_overloaded` and `sg_overutilized` values diverge:

**Case 1: Overutilized but NOT overloaded** — A single high-utilization task saturates a small (low-capacity) CPU, but no CPU has more than one runnable task. Here `sg_overutilized = true` and `sg_overloaded = false`. The bug causes `rd->overutilized = false` (should be `true`). This means the system remains in the EAS path even though some CPUs are overutilized. The EAS heuristic may make suboptimal decisions when the system is beyond its energy-efficient operating point, potentially failing to migrate the heavy task to a higher-capacity CPU because `find_energy_efficient_cpu()` does not consider load imbalance.

**Case 2: Overloaded but NOT overutilized** — Multiple tasks are queued on one CPU, but total utilization across all CPUs stays below their respective capacities. Here `sg_overloaded = true` and `sg_overutilized = false`. The bug causes `rd->overutilized = true` (should be `false`). This disables EAS unnecessarily, forcing the scheduler to fall back to the generic load-balancing heuristic, which does not consider energy efficiency. On heterogeneous (big.LITTLE) systems, this leads to increased power consumption because tasks that could be efficiently placed on small cores may instead be spread to big cores by the non-energy-aware balancer.

Both cases result in incorrect EAS behavior on heterogeneous CPU systems. The practical impact is energy inefficiency on mobile, embedded, and server platforms with asymmetric CPU capacities (e.g., ARM big.LITTLE, DynamIQ). There are no crashes or kernel warnings — the bug is a silent scheduling quality regression. The mailing list thread shows the patch was reviewed and accepted without any specific bug reports, indicating it was caught by code review inspection rather than user-visible symptoms.

## Fix Summary

The fix is a one-character change: replacing `sg_overloaded` with `sg_overutilized` as the argument to `set_rd_overutilized()` in the root-domain path of `update_sd_lb_stats()`:

```c
if (!env->sd->parent) {
    set_rd_overloaded(env->dst_rq->rd, sg_overloaded);
-   set_rd_overutilized(env->dst_rq->rd, sg_overloaded);
+   set_rd_overutilized(env->dst_rq->rd, sg_overutilized);
} else if (sg_overutilized) {
    set_rd_overutilized(env->dst_rq->rd, sg_overutilized);
}
```

This restores the correct semantics: the `rd->overutilized` flag at the root scheduling domain reflects whether any CPU in the system is overutilized (utilization exceeds capacity), not whether any CPU has multiple runnable tasks. The fix was authored by Vincent Guittot and reviewed by Shrikanth Hegde (who also reviewed the original refactoring commit). It was merged by Ingo Molnar into the `sched/core` branch of the tip tree.

The fix is correct and complete because it is the only call site where `sg_overloaded` was mistakenly passed to `set_rd_overutilized()`. The `else if` branch (for non-root domains) already correctly uses `sg_overutilized`. All other uses of `sg_overloaded` and `sg_overutilized` in `update_sd_lb_stats()` and `update_sg_lb_stats()` were correctly converted in the original refactoring.

## Triggering Conditions

The bug requires all of the following conditions to be met simultaneously:

1. **Energy Aware Scheduling (EAS) must be enabled.** This requires `CONFIG_ENERGY_MODEL=y` in the kernel configuration, an energy model registered for the CPUs (typically via a cpufreq driver such as `cppc_cpufreq` or `arm_scmi`), and asymmetric CPU capacities (`SD_ASYM_CPUCAPACITY` flag in the sched domain hierarchy). Without EAS, `sched_energy_enabled()` returns false, `set_rd_overutilized()` is a no-op, `cpu_overutilized()` always returns false, and the bug has zero observable effect.

2. **Asymmetric CPU capacities.** The system must have CPUs with different maximum capacities (e.g., big.LITTLE ARM, or x86 hybrid with P-cores and E-cores with different `arch_scale_cpu_capacity()` values). This is needed for `cpu_overutilized()` to return true — a CPU is overutilized when its CFS utilization exceeds its capacity.

3. **Load balancing at the root scheduling domain.** The buggy code path is in the `if (!env->sd->parent)` branch of `update_sd_lb_stats()`, which is only entered when processing the top-level (root) scheduling domain. On most systems with a multi-level domain hierarchy (MC → NUMA or MC → PKG), the root domain is the highest-level domain. Load balancing at this level happens periodically (every `sd->balance_interval` jiffies, typically starting at 1ms and doubling up to `sd->max_interval`).

4. **Divergent overloaded/overutilized state.** To observe the bug's effect, the system must be in a state where `sg_overloaded != sg_overutilized`. The most natural scenario is a single CPU-intensive task running on a small (low-capacity) CPU, causing it to be overutilized but not overloaded (only one task on that CPU). Another scenario is multiple light tasks on one CPU (overloaded) while no CPU exceeds its capacity (not overutilized).

5. **Kernel version between `4475cd8bfd9b` and `cd18bec668bb6221a54f03d0b645b7aed841f825`.** Both commits were merged into the tip tree's `sched/core` branch during the v6.10 development cycle. The bug never existed in a released stable kernel — it was caught and fixed before v6.10-rc1 was tagged.

The bug is deterministic once the above conditions are met: every invocation of `update_sd_lb_stats()` at the root domain will set `rd->overutilized` to the wrong value. There is no race condition or timing sensitivity — it is a straightforward logic error that fires on every root-domain load balance pass.

## Reproduce Strategy (kSTEP)

Reproducing this bug with kSTEP requires enabling EAS and creating a workload scenario where the overloaded and overutilized states diverge. The primary challenge is that EAS requires `CONFIG_ENERGY_MODEL=y` and the `sched_energy_present` static key to be enabled. kSTEP does not currently have built-in EAS support, but this can be achieved with minor framework extensions. Below is a detailed step-by-step plan.

### Required kSTEP Extensions

1. **Enable `sched_energy_present` static key.** Import the static key via `KSYM_IMPORT(sched_energy_present)` (it is declared in `kernel/sched/sched.h`, which is included by kSTEP's `internal.h`). In the driver's `setup()` function, call `static_branch_enable(&sched_energy_present)`. This requires `CONFIG_ENERGY_MODEL=y` in the kernel build config — if this config option is not set, `sched_energy_enabled()` is hardcoded to return false and the static key does not exist. The kSTEP kernel config may need to be updated to include `CONFIG_ENERGY_MODEL=y`.

2. **Asymmetric CPU capacity setup.** Use `kstep_cpu_set_capacity()` to create CPUs with different capacities. For example, CPU 1 at full capacity (1024) and CPU 2 at reduced capacity (512). This simulates a big.LITTLE system. The sched domain hierarchy must include `SD_ASYM_CPUCAPACITY` for `cpu_overutilized()` to produce meaningful results. kSTEP's `kstep_topo_apply()` rebuilds sched domains, which should pick up the capacity asymmetry. If it does not automatically set `SD_ASYM_CPUCAPACITY`, this flag may need to be manually added to the sched domain via `KSYM_IMPORT`.

### Driver Design

**Topology:** Configure QEMU with at least 3 CPUs (CPU 0 reserved for the driver, CPU 1 as the "big" core at capacity 1024, CPU 2 as the "small" core at capacity 512).

**Setup phase (in `setup()`):**
1. Call `kstep_topo_init()`.
2. Configure a topology with CPUs 1 and 2 in the same MC domain (or at minimum, sharing a root domain).
3. Call `kstep_topo_apply()` to rebuild sched domains.
4. Call `kstep_cpu_set_capacity(1, 1024)` and `kstep_cpu_set_capacity(2, 512)`.
5. Import and enable `sched_energy_present`: `KSYM_IMPORT(sched_energy_present); static_branch_enable(&sched_energy_present);`.

**Run phase (in `run()`):**
1. Create one CFS task via `kstep_task_create()`.
2. Pin it to CPU 2 (the small core) via `kstep_task_pin(p, 2, 3)`.
3. Wake the task with `kstep_task_wakeup(p)`.
4. Run many ticks with `kstep_tick_repeat(1000)` to allow PELT utilization to ramp up. The task should accumulate high utilization on CPU 2.
5. After utilization ramp-up, the small CPU 2 should be overutilized (`cpu_overutilized(2)` returns true because utilization exceeds the 512 capacity), but NOT overloaded (only one runnable task on CPU 2, so `nr_running == 1`).
6. Trigger additional ticks to allow `rebalance_domains` to fire and invoke `update_sd_lb_stats()` at the root domain level. The `on_sched_softirq_end` callback can be used to check the state after load balancing.

**Observation phase (in `on_sched_softirq_end` or after ticks):**
1. Access the root domain via `cpu_rq(1)->rd` or `cpu_rq(2)->rd` (both share the same root domain).
2. Read `rd->overutilized` using `READ_ONCE(rd->overutilized)`.
3. Also verify the conditions: check that `cpu_overutilized(2)` returns true and that CPU 2 has exactly one runnable task (`cpu_rq(2)->nr_running == 1`).

**Expected results:**
- **On the buggy kernel:** `rd->overutilized` will be `false` (reflecting `sg_overloaded`, which is false because no CPU has `nr_running > 1`). Call `kstep_fail("rd->overutilized is false but sg_overutilized is true")`.
- **On the fixed kernel:** `rd->overutilized` will be `true` (correctly reflecting `sg_overutilized`, which is true because CPU 2 is overutilized). Call `kstep_pass("rd->overutilized correctly reflects sg_overutilized")`.

### Alternative Scenario (Reverse Direction)

To test the opposite mismatch (overloaded but not overutilized), create two light tasks pinned to CPU 1 (the big core, capacity 1024). With two tasks, `nr_running > 1` so `sg_overloaded = true`. But if their combined utilization stays below 1024, `cpu_overutilized(1)` returns false, so `sg_overutilized = false`. On the buggy kernel, `rd->overutilized` would incorrectly be `true`; on the fixed kernel, it would correctly be `false`. This provides an additional test vector.

### Logging Strategy

Add `pr_info()` logging for:
- `sg_overloaded` and `sg_overutilized` values after load balancing (these are local variables in `update_sd_lb_stats`, so they must be observed indirectly via `rd->overutilized` and `rd->overloaded`)
- `rd->overutilized` and `rd->overloaded` after each load balance pass
- `cpu_rq(cpu)->nr_running` for each CPU
- PELT utilization values: `cpu_util_cfs(cpu)` and `arch_scale_cpu_capacity(cpu)` for each CPU
- Return value of `cpu_overutilized(cpu)` for each CPU

### Potential Complications

1. **CONFIG_ENERGY_MODEL not enabled:** If the kSTEP kernel config does not include this option, the `sched_energy_present` symbol will not exist, and `cpu_overutilized()` will be hardcoded to return false. The kernel config must be updated.
2. **PELT ramp-up time:** PELT utilization takes many ticks (approximately 32ms time constant with ~345 samples to reach steady state). With 1ms ticks, approximately 345 ticks should bring utilization close to maximum. Use `kstep_tick_repeat(500)` to ensure sufficient ramp-up.
3. **Load balance interval:** Root domain load balancing may not fire on every tick. Use `kstep_tick_repeat()` with enough ticks and the `on_sched_balance_begin` callback to confirm when root-domain balancing occurs.
4. **SD_ASYM_CPUCAPACITY flag:** The sched domain must have this flag for EAS to consider the system heterogeneous. kSTEP's topology setup via `kstep_cpu_set_capacity()` should cause the sched domain builder to set this flag during `kstep_topo_apply()`, but this needs verification.
5. **Static key side effects:** Enabling `sched_energy_present` without a registered energy model means `find_energy_efficient_cpu()` will bail out early when `rcu_dereference(rd->pd)` returns NULL. This is acceptable for our test — we only need to observe `rd->overutilized`, not the full EAS task placement path.
