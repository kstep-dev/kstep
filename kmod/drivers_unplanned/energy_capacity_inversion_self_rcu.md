# Energy: Capacity Inversion Detection Missing RCU Lock and Self-Comparison

**Commit:** `da07d2f9c153e457e845d4dcfdd13568d71d18a4`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.2-rc6
**Buggy since:** v6.2-rc1 (introduced by commit 44c7b80bffc3 "sched/fair: Detect capacity inversion")

## Bug Description

The capacity inversion detection mechanism in `update_cpu_capacity()` was introduced in commit 44c7b80bffc3 to detect when thermal throttling causes a performance domain (PD) with originally lower or equal capacity to end up with higher effective capacity than another PD. This is called "capacity inversion" and is important for Energy Aware Scheduling (EAS) to make correct task placement decisions on heterogeneous (big.LITTLE) systems under thermal pressure.

The original implementation had three distinct bugs. First, it used `static_branch_unlikely(&sched_asym_cpucapacity)` as the entry guard instead of `sched_energy_enabled()`. Performance domains (`rq->rd->pd`) are only allocated and maintained when EAS is active (which requires an energy model, the schedutil cpufreq governor, and asymmetric CPU capacity). Using the broader `sched_asym_cpucapacity` check means the code can attempt to traverse performance domains on systems where they don't exist, as asymmetric capacity can be present without EAS enabled (e.g., when using a different cpufreq governor).

Second, the code called `rcu_dereference(rq->rd->pd)` without holding `rcu_read_lock()`. Performance domain lists are RCU-protected, and while `update_cpu_capacity()` is called during load balancing (via `update_group_capacity()`), the RCU read lock is not guaranteed to be held at all call sites. Traversing the PD list without RCU protection opens a window for use-after-free if the PD list is concurrently modified (e.g., during cpufreq governor changes or energy model updates).

Third, the loop that compares against other performance domains did not skip the CPU's own PD. This means the CPU compares its effective capacity (after thermal pressure) against its own PD's effective capacity. Since `cpumask_any()` might select a different CPU within the same PD, and PELT-tracked thermal pressure values can differ slightly between CPUs in the same PD (due to per-CPU PELT averaging rates), this self-comparison can produce false positive capacity inversion detections.

## Root Cause

The root cause lies in the `update_cpu_capacity()` function in `kernel/sched/fair.c`. When capacity inversion detection was added, the function was extended with a loop over all performance domains to compare each PD's thermal-adjusted capacity against the current CPU's PD.

**Bug 1 — Wrong guard condition:** The code used:
```c
if (static_branch_unlikely(&sched_asym_cpucapacity)) {
```
The `sched_asym_cpucapacity` static key is enabled whenever the system has CPUs with different `capacity_orig` values, regardless of whether EAS infrastructure (energy model + schedutil) is active. However, the performance domain list `rq->rd->pd` is only populated when `sched_energy_enabled()` returns true. On a system with asymmetric capacity but without EAS (e.g., using the `performance` or `powersave` cpufreq governor), `rq->rd->pd` would be NULL, and while the for loop would simply not execute, the `rcu_dereference()` call without the proper guard is still semantically incorrect and could confuse static analysis tools and future maintainers.

**Bug 2 — Missing RCU protection:** The original code performed:
```c
struct perf_domain *pd = rcu_dereference(rq->rd->pd);
```
directly in the variable declaration without first acquiring `rcu_read_lock()`. The `rcu_dereference()` macro is a documentation/enforcement mechanism that the pointer should only be dereferenced within an RCU read-side critical section. Without the lock, the PD list could be freed by a concurrent writer (e.g., `sched_energy_set()` rebuilding PDs) while this code is iterating over it, leading to a use-after-free. The `update_cpu_capacity()` function is called from `update_group_capacity()` during `update_sd_lb_stats()` in the load balancing path. While `load_balance()` holds `rcu_read_lock()`, the v1 patch initially added a `SCHED_WARN_ON(!rcu_read_lock_held())` assertion, but the final v3 patch chose to explicitly acquire and release `rcu_read_lock()` to guarantee safety regardless of the call path.

**Bug 3 — Self-comparison causing false positives:** The loop iterates over ALL performance domains including the one the current CPU belongs to:
```c
for (; pd; pd = pd->next) {
    struct cpumask *pd_span = perf_domain_span(pd);
    cpu = cpumask_any(pd_span);
    pd_cap_orig = arch_scale_cpu_capacity(cpu);
    // ... comparison logic ...
}
```
When the loop reaches the current CPU's own PD, `capacity_orig == pd_cap_orig` is always true (since all CPUs in a PD share the same `capacity_orig`). The code then compares:
```c
pd_cap = pd_cap_orig - thermal_load_avg(cpu_rq(cpu));
if (pd_cap > inv_cap) {
    rq->cpu_capacity_inverted = inv_cap;
    break;
}
```
Here, `inv_cap = capacity_orig - thermal_load_avg(rq)` is the current CPU's thermally-adjusted capacity, and `pd_cap` is calculated using `cpumask_any(pd_span)` which may select a different CPU in the same PD. If `thermal_load_avg()` differs slightly between CPUs in the same PD (due to per-CPU PELT decay timing), `pd_cap > inv_cap` could evaluate to true, causing a false capacity inversion detection on the CPU's own performance domain.

## Consequence

The most directly observable consequence is false positive capacity inversion detection. When `rq->cpu_capacity_inverted` is set erroneously, it signals to `cpu_in_capacity_inversion()` that this CPU's performance domain is effectively smaller than another PD. This information is consumed by `util_fits_cpu()` in the EAS task placement path (`find_energy_efficient_cpu()`) and in the uclamp-aware capacity fitting logic. False inversion detection causes the scheduler to make suboptimal task placement decisions — for example, it might avoid placing tasks on CPUs that are actually the best candidates, or it might relax uclamp constraints unnecessarily.

The missing RCU protection creates a potential use-after-free vulnerability. If performance domains are being rebuilt (e.g., due to cpufreq governor change or energy model update) concurrently with `update_cpu_capacity()`, the code could dereference freed memory. In the worst case, this could cause a kernel crash (NULL pointer dereference or accessing freed slab memory), data corruption, or a security vulnerability. In practice, PD rebuilds are rare events, making this race hard to trigger but not impossible.

The wrong guard condition (`sched_asym_cpucapacity` instead of `sched_energy_enabled()`) is the least impactful in isolation, since the for loop over a NULL PD list simply doesn't execute. However, it means the code does unnecessary work (computing `inv_cap`, calling `rcu_dereference` on a NULL pointer) on systems with asymmetric capacity but without EAS, and it masks the RCU protection issue since `rcu_dereference(NULL)` effectively does nothing observable.

## Fix Summary

The fix commit makes three targeted changes to `update_cpu_capacity()`:

1. **Changes the guard condition** from `static_branch_unlikely(&sched_asym_cpucapacity)` to `sched_energy_enabled()`. This ensures the capacity inversion detection code only executes when performance domains are actually available (EAS is active), which requires both an energy model and the schedutil governor. This is the correct condition because PDs only exist when EAS is enabled.

2. **Adds proper RCU protection** by wrapping the PD traversal in `rcu_read_lock()` / `rcu_read_unlock()`. The `rcu_dereference(rq->rd->pd)` call is now inside the RCU read-side critical section, and the `pd` variable declaration is separated from its assignment to accommodate the lock acquisition. This guarantees the PD list cannot be freed while being traversed. Note that the v1 patch used `SCHED_WARN_ON(!rcu_read_lock_held())` as an assertion, but the final fix (v3) chose to explicitly take the lock for robustness.

3. **Adds a self-comparison skip** by inserting a check at the top of the loop body:
```c
if (cpumask_test_cpu(cpu_of(rq), pd_span))
    continue;
```
This skips the current CPU's own performance domain, since a PD cannot be capacity-inverted against itself. This eliminates the false positive inversion detections caused by per-CPU PELT tracking differences within the same PD.

## Triggering Conditions

To trigger the self-comparison bug (bug #3, the most observable):
- The system must have asymmetric CPU capacity (big.LITTLE or similar heterogeneous topology) with at least two performance domains.
- Energy Aware Scheduling must be active: this requires `CONFIG_ENERGY_MODEL=y`, `CONFIG_CPU_FREQ=y`, `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y`, the schedutil cpufreq governor selected, and a registered energy model.
- Thermal pressure must be applied to CPUs in a performance domain. This typically comes from the thermal framework (`arch_set_thermal_pressure()`) which feeds into PELT-tracked `thermal_load_avg()`.
- The thermal pressure must differ slightly between CPUs within the same PD (due to PELT averaging timing). This happens naturally because `update_cpu_capacity()` is called per-CPU during load balancing, which doesn't happen simultaneously on all CPUs.
- The `update_cpu_capacity()` function must be called during `update_sd_lb_stats()` in the load balancing path, which happens on each load balance tick.

To trigger the RCU bug (bug #2):
- The above conditions must hold, plus a concurrent PD rebuild must happen while `update_cpu_capacity()` is traversing the PD list. PD rebuilds occur during cpufreq governor changes, energy model registration/deregistration, or sched domain rebuilds.
- This is a race condition with a very small window (microseconds) and requires precise timing.

To trigger the guard condition bug (bug #1):
- A system with asymmetric CPU capacities but WITHOUT EAS enabled (e.g., using the `performance` cpufreq governor instead of `schedutil`, or no energy model registered). In this case, the buggy code enters the if block but `rq->rd->pd` is NULL, so the for loop simply doesn't execute. The practical impact is wasted computation and a semantically incorrect `rcu_dereference(NULL)`.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?** The bug requires three pieces of infrastructure that kSTEP cannot provide:

   - **Performance domains (PDs):** PDs are created by `perf_domain_alloc()` during sched domain rebuild, but only when `sched_energy_enabled()` returns true. This in turn requires: (a) a registered energy model (`em_dev_register_perf_domain()`), which needs a cpufreq driver to provide OPP (Operating Performance Point) tables with power/frequency data, and (b) the schedutil cpufreq governor to be active (`CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y` and selected). QEMU does not emulate real cpufreq hardware, so no cpufreq driver is loaded, no energy model can be registered, and `sched_energy_enabled()` always returns false. Without PDs, the capacity inversion detection code never executes on either the buggy or fixed kernel.

   - **Thermal pressure:** The `thermal_load_avg()` function returns the PELT-tracked thermal pressure for a CPU. Thermal pressure is injected via `arch_set_thermal_pressure()` or `topology_set_thermal_pressure()`, which is called by hardware thermal drivers (e.g., `cpu_cooling`, `arm-scmi` thermal) in response to real thermal events. QEMU does not emulate thermal zones or thermal drivers, so `thermal_load_avg()` always returns 0 for all CPUs. With zero thermal pressure, `inv_cap == capacity_orig` and `pd_cap == pd_cap_orig`, making the capacity inversion condition impossible to trigger.

   - **Energy Aware Scheduling (EAS) stack:** Even if kSTEP could set CPU capacities via `kstep_cpu_set_capacity()` to create an asymmetric topology, the full EAS stack (energy model + schedutil + PDs) would still be missing. The observable consequence of the bug (incorrect `rq->cpu_capacity_inverted`) only matters for EAS task placement via `find_energy_efficient_cpu()`, which is itself gated on `sched_energy_enabled()`.

2. **WHAT would need to be added to kSTEP?** To support this class of EAS/thermal bugs, kSTEP would need:

   - A mechanism to register a fake energy model for each performance domain, providing synthetic OPP tables. This could be a function like `kstep_em_register(cpumask, nr_opps, opps[])` that calls `em_dev_register_perf_domain()` internally.
   - A mechanism to activate the schedutil cpufreq governor, or at least fake `sched_energy_enabled()` returning true. Since schedutil requires a cpufreq driver, this would likely require a fake cpufreq driver module.
   - A function to inject thermal pressure, such as `kstep_set_thermal_pressure(cpu, pressure)` wrapping `arch_set_thermal_pressure()`, followed by sufficient ticks for PELT averaging to reflect the new pressure.
   - These are not minor additions — they require emulating the cpufreq, energy model, and thermal frameworks, which is fundamental hardware infrastructure outside kSTEP's current architecture.

3. **Alternative reproduction methods:** This bug can be reproduced on real ARM big.LITTLE hardware (e.g., Arm Juno development board, Qualcomm Snapdragon mobile SoCs, or MediaTek Dimensity SoCs) by:
   - Booting with EAS enabled (schedutil governor, energy model registered via the platform's cpufreq driver).
   - Applying thermal pressure via the thermal framework: `echo <value> > /sys/devices/virtual/thermal/thermal_zone<N>/emul_temp` on platforms that support temperature emulation, or by running a CPU-intensive workload in a hot environment.
   - Monitoring `rq->cpu_capacity_inverted` via ftrace or a custom tracepoint to observe false positive inversion detections.
   - Alternatively, a QEMU-based approach could work if a fake cpufreq driver and fake energy model module were written and loaded alongside the test, but this is equivalent to the kSTEP changes described above.
