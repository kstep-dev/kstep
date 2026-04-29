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

Reproducing this bug with kSTEP requires enabling Energy Aware Scheduling (EAS) and creating a workload scenario where the `sg_overloaded` and `sg_overutilized` states diverge. The key principle of this strategy is that EAS must be enabled entirely through public, exported kernel APIs and kSTEP interfaces — never by directly writing to the `sched_energy_present` static key or other internal scheduler fields. EAS enables itself naturally when three conditions are met during a sched domain rebuild: (1) an Energy Model is registered for the CPUs, (2) asymmetric CPU capacities exist (`SD_ASYM_CPUCAPACITY`), and (3) all CPUs use the `schedutil` cpufreq governor. All three conditions can be established through exported kernel APIs (`em_dev_register_perf_domain()`, `cpufreq_register_driver()`) and kSTEP's topology APIs (`kstep_cpu_set_capacity()`, `kstep_topo_apply()`). Internal scheduler state (`rd->overutilized`, `rd->overloaded`, `cpu_rq()->nr_running`, PELT utilization) is only ever read for observation, never written.

### Kernel Configuration

The kernel must be built with the following options: `CONFIG_ENERGY_MODEL=y` to compile the Energy Model framework (making `em_dev_register_perf_domain()` available), `CONFIG_CPU_FREQ=y` to enable the cpufreq subsystem, `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y` to compile the `schedutil` governor, and `CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y` to make `schedutil` the default governor for newly created cpufreq policies. On x86, `arch_scale_freq_capacity` is defined (in `arch/x86/include/asm/topology.h`), which causes `arch_scale_freq_invariant()` to return `true` — satisfying one of the `sched_is_eas_possible()` prerequisites without any driver action. SMT must be absent; QEMU should be configured without hyperthreading (one thread per core) so that `sched_smt_active()` returns false. Configure QEMU with at least 3 CPUs (`-smp 3`): CPU 0 is reserved for the kSTEP driver, CPU 1 serves as the "big" core (capacity 1024), and CPU 2 serves as the "little" core (capacity 512).

### CPUFreq Driver Registration

EAS requires that every CPU in the root domain has a cpufreq policy governed by `schedutil` (checked by `cpufreq_ready_for_eas()` → `sugov_is_governor()`). Since QEMU does not expose real CPU frequency scaling hardware, the driver must register a minimal cpufreq driver programmatically using the exported `cpufreq_register_driver()` API (EXPORT_SYMBOL_GPL). Use `KSYM_IMPORT(cpufreq_register_driver)` to import the function. Define a static `struct cpufreq_driver` with a trivial `.init()` callback that populates the policy's frequency table with two synthetic OPPs (e.g., 1000 MHz and 2000 MHz for the big core, 500 MHz and 1000 MHz for the little core), a `.verify()` callback using the generic `cpufreq_generic_frequency_table_verify`, a `.target_index()` that is a no-op (returns 0), and a `.get()` that returns the current "frequency." Set `.flags = CPUFREQ_CONST_LOOPS` and `.attr = cpufreq_generic_attr`. Call `KSYM_cpufreq_register_driver(&kstep_cpufreq_driver)` in `setup()`. Because `CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y` is set, each CPU's cpufreq policy will automatically use `schedutil` as its governor once the driver's `.init()` creates the policy. This satisfies the `cpufreq_ready_for_eas()` check entirely through the public cpufreq API.

### Energy Model Registration

After the cpufreq driver is registered, the Energy Model must be registered for each performance domain so that `em_cpu_get(cpu)` returns a valid `struct em_perf_domain *` during the sched domain rebuild. Use `KSYM_IMPORT(em_dev_register_perf_domain)` (EXPORT_SYMBOL_GPL) and `KSYM_IMPORT(get_cpu_device)` (EXPORT_SYMBOL_GPL). Define an `active_power` callback that returns synthetic but valid power/frequency pairs: for the big core (CPU 1), return two states (e.g., 1000 MHz at 200 µW and 2000 MHz at 800 µW); for the little core (CPU 2), return two states (e.g., 500 MHz at 50 µW and 1000 MHz at 300 µW). Construct an `EM_DATA_CB(kstep_active_power)` and call `KSYM_em_dev_register_perf_domain(get_cpu_device(cpu), 2, &em_cb, cpumask, true)` for each performance domain. The `cpumask` should contain just the single CPU in each domain (CPU 1 alone, CPU 2 alone — each is its own perf domain in our simple topology). The `microwatts = true` parameter indicates the power values are in micro-watts. This registration is through the kernel's public Energy Model API and does not write to any internal scheduler data structure.

### Topology Setup and EAS Activation

With the cpufreq driver and Energy Model in place, set up the asymmetric topology. Call `kstep_cpu_set_capacity(1, SCHED_CAPACITY_SCALE)` (1024, big core) and `kstep_cpu_set_capacity(2, SCHED_CAPACITY_SCALE / 2)` (512, little core). Then call `kstep_topo_init()` and configure CPUs 1 and 2 in the same MC (machine check / cluster) domain: `const char *mc[] = {"0", "1-2"}; kstep_topo_set_mc(mc, 2);`. Finally, call `kstep_topo_apply()`, which internally calls `rebuild_sched_domains()`. During the rebuild, `build_sched_domains()` detects the capacity asymmetry and sets the `SD_ASYM_CPUCAPACITY` flag on the appropriate sched domain, populating the per-CPU `sd_asym_cpucapacity` pointer. Then `build_perf_domains()` runs, calls `sched_is_eas_possible()` (all checks pass: asymmetric capacities present, no SMT, freq-invariant, schedutil on all CPUs), and calls `pd_init()` for each CPU which succeeds because `em_cpu_get()` returns the registered EM. The result is that `sched_energy_set(true)` is called internally, enabling the `sched_energy_present` static key through the scheduler's own natural code path — with zero direct writes to the static key from the driver.

### Workload Design — Case 1: Overutilized but NOT Overloaded

The primary test scenario creates a state where `sg_overutilized = true` and `sg_overloaded = false`, exposing the bug where `rd->overutilized` is incorrectly set to `false`. In `run()`, create a single CFS task via `kstep_task_create()` and pin it to CPU 2 (the little core, capacity 512) with `kstep_task_pin(p, 2, 3)`. Wake it with `kstep_task_wakeup(p)`. Run `kstep_tick_repeat(500)` to allow PELT utilization to ramp up. The PELT geometric series has a ~32ms time constant; with kSTEP's 1ms tick interval, approximately 345 ticks bring utilization close to steady state, so 500 ticks provides ample margin. After ramp-up, CPU 2's CFS utilization will approach `SCHED_CAPACITY_SCALE` (1024), which far exceeds its capacity of 512. The function `cpu_overutilized(2)` — which checks `cpu_util_cfs(cpu) > effective_cpu_capacity(cpu)` — will return true. Meanwhile, CPU 2 has only one runnable task (`nr_running == 1`), so the `sg_overloaded` flag (set when any CPU has `nr_running > 1`) remains false. This is the critical divergence: `sg_overutilized = true, sg_overloaded = false`.

### Workload Design — Case 2: Overloaded but NOT Overutilized

As a complementary test, create the reverse mismatch. Pin two lightweight tasks to CPU 1 (the big core, capacity 1024) using `kstep_task_pin()`. Wake both, then run only a modest number of ticks (e.g., `kstep_tick_repeat(100)`) — enough for load balancing to fire but not enough for PELT utilization to ramp up significantly. With two runnable tasks on CPU 1, `nr_running > 1`, so `sg_overloaded = true`. But the combined utilization of two newly-woken tasks is still well below CPU 1's capacity of 1024, so `cpu_overutilized(1)` returns false and `sg_overutilized = false`. On the buggy kernel, `rd->overutilized` would be incorrectly set to `true` (from `sg_overloaded`), disabling EAS unnecessarily. On the fixed kernel, `rd->overutilized` would correctly be `false`. Alternatively, use `kstep_task_pause()` and `kstep_task_wakeup()` to keep utilization low while maintaining two runnable tasks.

### Triggering Root-Domain Load Balancing

The buggy code executes only in the `if (!env->sd->parent)` branch of `update_sd_lb_stats()`, which is reached when the load balancer processes the root (top-level) scheduling domain. This happens periodically during `rebalance_domains()`, which is invoked from the scheduler softirq. The balance interval for the root domain starts at `sd->balance_interval` (typically 1–4ms) and can double up to `sd->max_interval`. After the workload is set up, call `kstep_tick_repeat(100)` or use `kstep_tick_until(fn)` with a callback that checks whether root-domain load balancing has occurred. The `on_sched_balance_begin` callback can be used to detect when `update_sd_lb_stats()` fires: inside the callback, check `env->sd->parent == NULL` to confirm it is the root domain. Once root-domain balancing has executed at least once after PELT utilization reaches steady state, the `rd->overutilized` flag will reflect the (potentially incorrect) value.

### Observation and Verification

All observation is read-only. In the `on_sched_softirq_end` callback (or after the tick loop completes), read the root domain's state: `struct root_domain *rd = cpu_rq(1)->rd;` then `bool overut = READ_ONCE(rd->overutilized);` and `bool overld = READ_ONCE(rd->overloaded);`. Also verify the preconditions by reading `cpu_rq(2)->nr_running` (should be 1 for Case 1) and checking `cpu_overutilized(2)` by importing it via `KSYM_IMPORT(cpu_overutilized)` or computing it inline by comparing `cpu_util_cfs(2)` against `arch_scale_cpu_capacity(2)`. For Case 1 (overutilized, not overloaded): on the **buggy kernel**, `rd->overutilized` will be `false` (incorrectly reflecting `sg_overloaded`), so call `kstep_fail("rd->overutilized=false but system is overutilized (cpu2 util > capacity)")`. On the **fixed kernel**, `rd->overutilized` will be `true`, so call `kstep_pass("rd->overutilized correctly reflects sg_overutilized")`. For Case 2 (overloaded, not overutilized): on the buggy kernel, `rd->overutilized` will be `true` (incorrectly); on the fixed kernel, `false` (correctly). Log all values via `kstep_pass()`/`kstep_fail()` with descriptive format strings including the actual values of `overutilized`, `overloaded`, `nr_running`, and utilization.

### Logging Strategy

Add `pr_info()` logging for: `rd->overutilized` and `rd->overloaded` values after each root-domain load balance pass (read-only); `cpu_rq(cpu)->nr_running` for CPUs 1 and 2; PELT utilization via `cpu_util_cfs(cpu)` and capacity via `arch_scale_cpu_capacity(cpu)` for each CPU; and the return value of `cpu_overutilized(cpu)`. Since `sg_overloaded` and `sg_overutilized` are local variables inside `update_sd_lb_stats()` and cannot be directly observed, they are inferred indirectly: `sg_overloaded` corresponds to `rd->overloaded` (which is set correctly from `sg_overloaded` even in the buggy kernel), and `sg_overutilized` can be computed by checking `cpu_overutilized()` for each CPU. This allows the driver to log the full picture and confirm the divergence.

### Potential Complications

1. **cpufreq driver complexity:** The dummy cpufreq driver must provide a valid `struct cpufreq_frequency_table` and implement `.init()` to set `policy->cpuinfo.min_freq`, `policy->cpuinfo.max_freq`, and `policy->freq_table`. If the table is malformed, `cpufreq_table_validate_and_sort()` will reject it. Use simple, monotonically increasing frequencies (e.g., 1000000 and 2000000 kHz) with `CPUFREQ_TABLE_END` sentinel.
2. **EM registration validation:** `em_dev_register_perf_domain()` validates that power increases with frequency and that frequencies are unique and sorted. Ensure the `active_power` callback returns consistent, monotonically increasing power for increasing frequencies.
3. **PELT ramp-up time:** Approximately 345 ticks (at 1ms intervals) are needed for PELT to reach ~95% of steady state. Use at least 500 ticks to ensure utilization fully exceeds the little core's capacity.
4. **`SD_ASYM_CPUCAPACITY` detection:** The sched domain builder sets this flag when `arch_scale_cpu_capacity()` returns different values for CPUs in the same domain. Since `kstep_cpu_set_capacity()` sets per-CPU capacity before `kstep_topo_apply()` rebuilds domains, the flag should be set automatically. Verify by reading `cpu_rq(1)->sd` and checking for `SD_ASYM_CPUCAPACITY` in the domain flags (read-only).
5. **Order of operations:** The cpufreq driver must be registered first (so `schedutil` is governing all CPUs), then the EM (so `em_cpu_get()` succeeds), then capacities set and `kstep_topo_apply()` called. If the order is wrong, `build_perf_domains()` will fail one of its prerequisite checks and EAS will not activate.
6. **EAS activation verification:** After `kstep_topo_apply()`, verify that EAS is active by checking `cpu_rq(1)->rd->pd != NULL` (read-only). If `rd->pd` is NULL, `build_perf_domains()` failed, and the driver should call `kstep_fail()` with a diagnostic message rather than proceeding to the workload phase.
