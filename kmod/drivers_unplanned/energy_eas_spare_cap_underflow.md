# Energy: Unsigned Underflow in EAS Spare Capacity Calculation

**Commit:** `da0777d35f47892f359c3f73ea155870bb595700`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.10-rc1
**Buggy since:** v5.6-rc1 (introduced by commit `1d42509e475cd` "sched/fair: Make EAS wakeup placement consider uclamp restrictions")

## Bug Description

The Energy Aware Scheduler (EAS) in `find_energy_efficient_cpu()` computes the spare capacity of each CPU to determine the best placement for a waking CFS task. Spare capacity is calculated as `cpu_cap - util`, where `cpu_cap` is the current CPU capacity (reduced by RT/DL pressure, IRQ time, and thermal throttling) and `util` is the estimated CPU utilization if the task were placed there. This spare capacity value is used to rank CPUs within each performance domain — the CPU with the highest spare capacity in each domain is selected as the candidate for energy computation.

The bug occurs because `cpu_cap` can be less than `util` under certain conditions. The CPU's effective capacity (`capacity_of(cpu)`) is reduced by higher-priority scheduling classes (RT, DL), IRQ time, and thermal pressure. Meanwhile, `cpu_util_next()` computes the projected CFS utilization including the waking task, which does not account for these capacity reductions. When the effective capacity drops below the projected utilization, the subtraction `cpu_cap - util` produces a massive positive value because both operands are `unsigned long` — the result wraps around to near `ULONG_MAX` instead of being negative.

This unsigned underflow is particularly problematic because the buggy commit `1d42509e475cd` reorganized the code so that `spare_cap` is computed *before* the `fits_capacity()` check. Previously, the capacity fitness check filtered out overutilized CPUs before spare capacity was used. After the uclamp-aware rework, `spare_cap` is computed first, then `util` is modified by `uclamp_rq_util_with()` before the `fits_capacity()` test. This means a CPU where `cpu_cap < util` (in raw terms) might still pass the `fits_capacity()` check after uclamp reduces `util`, but its `spare_cap` value has already been computed with the raw (higher) `util` and has underflowed.

## Root Cause

The root cause is a plain unsigned integer underflow in the expression `spare_cap = cpu_cap - util` at line 6596 of `kernel/sched/fair.c` (in the v5.9-rc1 tree).

The code flow in the buggy version is:

```c
util = cpu_util_next(cpu, p, cpu);   // raw CFS util with task p placed on cpu
cpu_cap = capacity_of(cpu);          // effective capacity (reduced by RT/DL/IRQ/thermal)
spare_cap = cpu_cap - util;          // BUG: unsigned underflow if cpu_cap < util

// Now modify util with uclamp for the capacity fitness check
util = uclamp_rq_util_with(cpu_rq(cpu), util, p);
if (!fits_capacity(util, cpu_cap))
    continue;

// spare_cap (possibly underflowed) is used to rank CPUs
if (spare_cap > max_spare_cap) {
    max_spare_cap = spare_cap;
    max_spare_cap_cpu = cpu;
}
```

The key insight is the ordering: `spare_cap` is calculated using the raw `util` value from `cpu_util_next()`, but the `fits_capacity()` guard uses a *different* `util` value that has been adjusted by `uclamp_rq_util_with()`. The `uclamp_rq_util_with()` function can significantly reduce `util` by applying uclamp max restrictions from both the task and the runqueue. This means a CPU can pass the fitness check (because the uclamp-adjusted util is small enough) while simultaneously having an underflowed `spare_cap` (because the raw util exceeded the effective capacity).

Before commit `1d42509e475cd`, the code computed `spare_cap` *after* the `fits_capacity()` check, and both used the same `util` value. That commit moved the spare capacity computation before the fitness check and introduced a second `util` computation with `uclamp_rq_util_with()`, creating the window for the underflow.

The `capacity_of(cpu)` function returns `cpu_rq(cpu)->cpu_capacity`, which is the CPU's original capacity minus time consumed by RT tasks, DL tasks, and IRQ handlers. On a big.LITTLE system, a LITTLE CPU might have an original capacity of 512. With RT tasks consuming some capacity, this could drop to, say, 400. If CFS utilization (including the waking task) is 450, then `spare_cap = 400 - 450` underflows to approximately `ULONG_MAX - 49`, an astronomically large number.

## Consequence

The underflowed `spare_cap` value (near `ULONG_MAX`) is compared against `max_spare_cap` (initialized to 0) in the ranking logic. Since the underflowed value is enormous, the CPU with the underflowed spare capacity will almost certainly be selected as the `max_spare_cap_cpu` — the CPU believed to have the most spare capacity in that performance domain.

This leads to incorrect CPU selection in EAS. A CPU that is actually overloaded (raw utilization exceeds effective capacity) is chosen as the best energy-efficient target. The consequences include:

1. **Suboptimal task placement**: Tasks are placed on CPUs that are already heavily loaded, leading to performance degradation and increased energy consumption — the exact opposite of what EAS is designed to achieve.
2. **Potential overutilization**: Placing a task on an already-overloaded CPU can drive it further into overutilization, potentially triggering the overutilized flag for the entire root domain, which disables EAS entirely and falls back to the load balancer.
3. **Energy waste on asymmetric systems**: On big.LITTLE/DynamIQ platforms, this bug can cause tasks to be placed on big cores unnecessarily (because a big core with underflowed spare_cap looks more attractive than a properly-computed LITTLE core), wasting energy and battery life.

The bug does not cause a kernel crash or data corruption, but it silently corrupts the EAS placement decision, leading to measurable performance and energy regressions on affected platforms.

## Fix Summary

The fix replaces the direct subtraction `spare_cap = cpu_cap - util` with a safe subtraction using the `lsub_positive()` macro:

```c
spare_cap = cpu_cap;
lsub_positive(&spare_cap, util);
```

The `lsub_positive()` macro is defined as:

```c
#define lsub_positive(_ptr, _val) do {
    typeof(_ptr) ptr = (_ptr);
    *ptr -= min_t(typeof(*ptr), *ptr, _val);
} while (0)
```

This computes `*ptr = *ptr - min(*ptr, _val)`, which effectively clamps the result to zero if `_val > *ptr`. So when `util > cpu_cap`, `spare_cap` becomes 0 instead of underflowing. A `spare_cap` of 0 correctly represents "no spare capacity" and will not win the `spare_cap > max_spare_cap` comparison (since `max_spare_cap` is initialized to 0 and the comparison is strict `>`), meaning the overloaded CPU will not be selected as the best candidate.

This is the correct and minimal fix. It preserves the existing code structure and ordering — the spare capacity is still computed before the uclamp-adjusted fitness check — but ensures the arithmetic is safe. The fix aligns with common kernel practice where `lsub_positive()` is used throughout the scheduler for unsigned subtraction that might underflow.

## Triggering Conditions

To trigger this bug, the following conditions must all be met simultaneously:

1. **Asymmetric CPU capacity system (big.LITTLE/DynamIQ)**: EAS is only active on systems with asymmetric CPU capacities. The system must have performance domains with differing CPU capacities (e.g., ARM big.LITTLE SoC).

2. **EAS enabled and not overutilized**: The root domain must not be marked as overutilized (`rd->overutilized == false`), otherwise EAS is bypassed entirely and `find_energy_efficient_cpu()` returns early.

3. **CONFIG_UCLAMP_TASK enabled**: The bug is specifically triggered by the interaction between raw utilization and uclamp-adjusted utilization. Without uclamp, the `fits_capacity()` check would use the same `util` value as the spare_cap computation, and any CPU where `util > cpu_cap` would be filtered out before spare_cap is compared.

4. **Reduced CPU capacity**: The target CPU's effective capacity must be reduced below the raw CFS utilization. This happens when:
   - RT or DL tasks are consuming CPU time (reducing `cpu_rq->cpu_capacity`)
   - Significant IRQ time is consumed on the CPU
   - Thermal throttling has reduced the CPU's capacity
   
5. **uclamp reducing util below capacity**: After the raw `util` exceeds `cpu_cap`, `uclamp_rq_util_with()` must reduce `util` enough that it passes `fits_capacity(util, cpu_cap)`. This requires the task's `uclamp.max` (or the runqueue's aggregated `uclamp.max`) to be set low enough to clamp the utilization below the capacity threshold. For example, if `cpu_cap = 400` and raw `util = 450`, uclamp needs to bring `util` down to below approximately `400 * 1024/1280 = 320` (the `fits_capacity` threshold).

6. **CFS task wakeup**: The bug is in the wakeup path (`find_energy_efficient_cpu()` called from `select_task_rq_fair()`), so a CFS task must be waking up and going through EAS placement.

The bug is deterministic given the right capacity and utilization state — it is not a race condition. However, it requires a specific combination of workload characteristics (RT pressure + uclamp configuration + appropriate CFS utilization levels) that may be rare in practice but is entirely possible on mobile/embedded platforms where uclamp is actively used for power management.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reason:

1. **Kernel version too old**: The fix commit `da0777d35f47892f359c3f73ea155870bb595700` was merged into the `sched/core` branch of the tip tree and shipped in **v5.10-rc1**. kSTEP only supports Linux **v5.15 and newer**. The bug was introduced in v5.6-rc1 (commit `1d42509e475cd`) and fixed in v5.10-rc1, so the entire window of affected kernels (v5.6 through v5.9.x) falls outside kSTEP's supported range.

2. **What would be needed if the version were supported**: If the kernel version were within kSTEP's supported range, reproducing this bug would require:
   - An asymmetric CPU topology (big.LITTLE) configured via `kstep_topo_*` APIs with different capacity levels.
   - RT tasks pinned to specific CPUs to reduce their effective capacity via `kstep_task_fifo()` and `kstep_task_pin()`.
   - Uclamp configuration to set task-level `uclamp.max` values low enough that `uclamp_rq_util_with()` reduces util below the capacity threshold. This would require extending kSTEP with a `kstep_task_set_uclamp()` API or writing uclamp values via the cgroup interface using `kstep_sysctl_write()`.
   - A CFS task wakeup that goes through the EAS path, triggered by `kstep_task_wakeup()`.
   - Observation of the selected CPU to verify incorrect placement via `kstep_output_curr_task()` or by reading the task's `cpu` field.

3. **Alternative reproduction methods**: The bug could be reproduced on a real big.LITTLE ARM platform (e.g., Juno, HiKey960, or a Snapdragon/Exynos mobile SoC) running a kernel in the v5.6-v5.9 range with:
   - `CONFIG_ENERGY_MODEL=y`, `CONFIG_UCLAMP_TASK=y`, `CONFIG_UCLAMP_TASK_GROUP=y`
   - RT tasks pinned to target CPUs to reduce effective capacity
   - CFS tasks with `uclamp.max` set via `sched_setattr()` syscall
   - Tracing via `ftrace` or `trace-cmd` on the `sched_wakeup` and `sched_migrate_task` tracepoints to observe incorrect CPU selection
   - Alternatively, adding `pr_debug()` prints in `find_energy_efficient_cpu()` to log `spare_cap`, `util`, and `cpu_cap` values to detect the underflow

4. **QEMU limitation note**: Even with the correct kernel version, QEMU does not typically model asymmetric CPU capacities natively. kSTEP works around this with its topology APIs, but real energy model data and perf domain configuration would need to be properly set up for EAS to activate.
