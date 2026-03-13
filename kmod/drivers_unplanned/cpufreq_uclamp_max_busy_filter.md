# Cpufreq: Schedutil busy filter ignores uclamp_max frequency capping

**Commit:** `7a17e1db1265471f7718af100cfc5e41280d53a7`
**Affected files:** kernel/sched/cpufreq_schedutil.c, kernel/sched/sched.h
**Fixed in:** v5.18-rc1
**Buggy since:** v5.3-rc1 (introduced by `982d9cdc22c9` "sched/cpufreq, sched/uclamp: Add clamps for FAIR and RT tasks")

## Bug Description

The schedutil cpufreq governor contains a "busy" filter in its single-CPU update paths (`sugov_update_single_freq()` and `sugov_update_single_perf()`) that prevents the CPU frequency from being reduced when the CPU has had no idle time recently. The rationale is that reducing frequency on a busy CPU is likely premature — the CPU is clearly saturated and should stay at the current (higher) frequency.

However, this filter is unconditional: it fires whenever `sugov_cpu_is_busy()` returns true and the new target frequency/performance is lower than the current one. When `uclamp_max` is used to cap a task's maximum utilization (and thus limit the CPU frequency the schedutil governor should select), this busy filter overrides the uclamp capping. The result is that `uclamp_max` becomes completely ineffective for frequency control on systems using the single-CPU schedutil path — the frequency stays at its current (high) level regardless of the uclamp_max constraint.

This is a significant problem for power management and thermal control. The `uclamp_max` mechanism was specifically designed to allow administrators or applications to cap CPU frequency for tasks, enabling use cases such as preventing thermal throttling, conserving battery, or limiting performance of background workloads. With this bug, those controls are silently ignored whenever the CPU is fully utilized, which is precisely the scenario where frequency capping matters most.

The bug affects all systems using the schedutil cpufreq governor on single-policy-per-CPU configurations (the `sugov_update_single_freq` and `sugov_update_single_perf` code paths), which includes most x86 and many ARM platforms. Multi-CPU shared-policy configurations (`sugov_next_freq_shared`) are not affected because that path does not have the busy filter.

## Root Cause

The root cause is in two functions in `kernel/sched/cpufreq_schedutil.c`:

**In `sugov_update_single_freq()`**, after computing the next target frequency via `get_next_freq()` (which already accounts for uclamp clamping through `effective_cpu_util()` → `uclamp_rq_util_with()`), the code checks:

```c
if (sugov_cpu_is_busy(sg_cpu) && next_f < sg_policy->next_freq) {
    next_f = sg_policy->next_freq;
    sg_policy->cached_raw_freq = cached_freq;
}
```

This unconditionally overrides the computed next frequency with the current (higher) frequency whenever the CPU is busy and the new frequency would be lower. Even though `get_next_freq()` correctly computed a lower frequency respecting uclamp_max, the busy filter throws that away.

**In `sugov_update_single_perf()`**, the analogous check is:

```c
if (sugov_cpu_is_busy(sg_cpu) && sg_cpu->util < prev_util)
    sg_cpu->util = prev_util;
```

This prevents the utilization target from being reduced when the CPU is busy, even though `sugov_get_util()` → `effective_cpu_util()` → `uclamp_rq_util_with()` had already clamped the utilization down to respect uclamp_max.

The function `sugov_cpu_is_busy()` returns true when the CPU had no idle time in the previous tick interval (i.e., `sg_cpu->idle_calls` was zero or decreased). A CPU-bound task capped by uclamp_max is inherently busy — it uses 100% of its time slice. Therefore, `sugov_cpu_is_busy()` returns true, and the busy filter fires on every single schedutil update, permanently blocking any frequency reduction requested by uclamp_max.

The fundamental logical error is that the busy filter was designed for the pre-uclamp world where "busy CPU" unambiguously meant "CPU needs maximum performance." With uclamp_max, a busy CPU might intentionally need to run at a lower frequency. The filter lacks awareness of this distinction.

## Consequence

The observable impact is that `uclamp_max` has no effect on CPU frequency when the affected task is actively running and keeping the CPU busy. Specifically:

1. **Frequency capping completely ignored**: Running `uclampset -M 0 sysbench --test=cpu --threads=4` on a 2-core SMT2 system produces a benchmark score of ~3200 (maximum), identical to running without any uclamp constraint. The CPU runs at its maximum frequency (e.g., 3.1 GHz) despite uclamp_max being set to 0.

2. **Power and thermal management broken**: Using `uclampset -M 400` during a kernel compilation has no effect — the CPU stays at maximum 3.1 GHz. This defeats the purpose of uclamp_max for constraining power consumption and preventing thermal throttling on battery-powered or thermally-constrained devices.

3. **Silent failure**: There is no error, warning, or indication that uclamp_max is being ignored. The system simply behaves as if no frequency cap were applied. Users and system administrators may believe their power management policy is active when it is not.

The bug is most impactful on mobile, embedded, and laptop systems where power management is critical, and where the schedutil governor's single-CPU path is the default configuration. ARM-based platforms (phones, tablets, Chromebooks) and Intel laptops are the primary affected systems.

## Fix Summary

The fix adds a guard condition `!uclamp_rq_is_capped(cpu_rq(sg_cpu->cpu))` before the busy filter in both `sugov_update_single_freq()` and `sugov_update_single_perf()`. The modified conditions become:

```c
// In sugov_update_single_freq():
if (!uclamp_rq_is_capped(cpu_rq(sg_cpu->cpu)) &&
    sugov_cpu_is_busy(sg_cpu) && next_f < sg_policy->next_freq) {

// In sugov_update_single_perf():
if (!uclamp_rq_is_capped(cpu_rq(sg_cpu->cpu)) &&
    sugov_cpu_is_busy(sg_cpu) && sg_cpu->util < prev_util)
```

When `uclamp_rq_is_capped()` returns true (meaning the runqueue's combined CFS+RT utilization exceeds the `uclamp_max` value), the busy filter is bypassed entirely, allowing the frequency to be reduced as dictated by the uclamp capping.

A new helper function `uclamp_rq_is_capped()` is introduced in `kernel/sched/sched.h`:

```c
static inline bool uclamp_rq_is_capped(struct rq *rq)
{
    unsigned long rq_util;
    unsigned long max_util;

    if (!static_branch_likely(&sched_uclamp_used))
        return false;

    rq_util = cpu_util_cfs(cpu_of(rq)) + cpu_util_rt(rq);
    max_util = READ_ONCE(rq->uclamp[UCLAMP_MAX].value);

    return max_util != SCHED_CAPACITY_SCALE && rq_util >= max_util;
}
```

This function returns true when: (a) uclamp is actively used, (b) `uclamp_max` is set below the maximum scale (1024), and (c) the combined CFS and RT utilization meets or exceeds the `uclamp_max` value. This correctly identifies the condition where uclamp_max is actively constraining the runqueue. The fix also shuffles some code in `sched.h` to move `cpu_util_cfs()` and `cpu_util_rt()` definitions above the uclamp functions so they can be called from `uclamp_rq_is_capped()`. A no-op stub returning `false` is provided when `CONFIG_UCLAMP_TASK` is disabled.

The fix is correct because it only disables the busy filter when uclamp_max is actively capping the runqueue. In all other cases (uclamp not used, uclamp_max at max, or utilization below uclamp_max), the busy filter operates exactly as before, preserving the original anti-premature-reduction behavior.

## Triggering Conditions

The following conditions are all required to trigger this bug:

1. **Kernel configuration**: `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y` (schedutil governor compiled in), `CONFIG_UCLAMP_TASK=y` (uclamp support enabled), `CONFIG_CPU_FREQ=y` (cpufreq subsystem enabled), and `CONFIG_SMP=y`.

2. **Active cpufreq driver**: A cpufreq driver must be registered and active so that the schedutil governor can be selected and its callbacks invoked. Without a driver, the `cpufreq_update_util_data` per-CPU pointer is NULL and `cpufreq_update_util()` is a no-op.

3. **Schedutil governor active**: The schedutil governor must be the active governor for the cpufreq policy. The bug is in the `sugov_update_single_freq()` and `sugov_update_single_perf()` paths, which are used when each CPU has its own cpufreq policy (as opposed to shared policies).

4. **uclamp_max set below current utilization**: A task must have its `uclamp_max` set to a value lower than the CPU's current utilization. This can be done via `sched_setattr()` syscall or the `uclampset` command-line tool (available in util-linux v2.37.2+). For example, `uclampset -M 0` sets uclamp_max to 0, or `uclampset -M 400` sets it to ~39% of max capacity.

5. **CPU is busy**: The capped task must be CPU-bound (or the CPU must have no idle time), so that `sugov_cpu_is_busy()` returns true. This is the common case for compute-bound workloads that uclamp_max is designed to constrain.

6. **Single-CPU policy path**: The system must use per-CPU cpufreq policies (not shared across multiple CPUs), so that the `sugov_update_single_freq` / `sugov_update_single_perf` paths are used rather than `sugov_next_freq_shared`.

The bug is 100% reproducible when all conditions are met — it occurs on every schedutil update cycle whenever the CPU is busy and uclamp_max is active.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. The reasons are:

### 1. Why this bug cannot be reproduced with kSTEP

The bug is in the schedutil cpufreq governor (`kernel/sched/cpufreq_schedutil.c`), specifically in `sugov_update_single_freq()` and `sugov_update_single_perf()`. These functions are callback handlers registered via `cpufreq_add_update_util_data()` and invoked through `cpufreq_update_util()` during scheduler ticks and task state changes.

The entire cpufreq subsystem requires a registered cpufreq driver to be functional. Without a driver:
- No cpufreq policy is created
- No governor is attached or started
- The per-CPU `cpufreq_update_util_data` pointer remains NULL
- `cpufreq_update_util()` (called from `update_curr()`, `enqueue_entity()`, etc.) becomes a no-op
- The buggy code in `sugov_update_single_freq()` / `sugov_update_single_perf()` is never reached

QEMU does not provide any cpufreq hardware or emulated frequency scaling interface. There is no virtual cpufreq driver available in the QEMU/KVM virtualized environment that kSTEP runs in. The `kstep_cpu_set_freq()` API in kSTEP sets the arch topology frequency scale factor (`arch_set_freq_scale`), but does not register a cpufreq driver or enable the schedutil governor.

### 2. What would need to be added to kSTEP

To support reproducing this bug, kSTEP would need fundamental additions:

- **A virtual cpufreq driver**: A kernel module that registers a `struct cpufreq_driver` with the cpufreq subsystem, advertising virtual frequency OPPs (operating performance points). This driver would need to implement `target()` or `fast_switch()` callbacks and maintain state about the "current" frequency for each CPU.

- **Governor activation**: After the driver registers, the schedutil governor must be selected (e.g., via `cpufreq_set_policy()` or sysctl write to `/sys/devices/system/cpu/cpufreq/policy0/scaling_governor`). This triggers governor initialization which sets up the `sugov_cpu` per-CPU structures and registers the `cpufreq_update_util_data` callback.

- **Frequency observation mechanism**: kSTEP would need a way to read back the frequency that the schedutil governor requests (the `next_freq` or the arguments to `cpufreq_driver_fast_switch()`). The virtual driver's `fast_switch()` callback could log or expose the requested frequency.

- **uclamp_max configuration for tasks**: kSTEP would need an API like `kstep_task_set_uclamp_max(p, value)` to set the uclamp_max for a task. This requires calling `sched_setattr()` or directly manipulating the task's `uclamp_req[UCLAMP_MAX]` field and the per-rq uclamp aggregation.

This constitutes a significant architectural addition — essentially building a virtual cpufreq subsystem — rather than a minor hook or helper function. It would affect multiple kernel subsystems (cpufreq core, cpufreq driver interface, governor lifecycle) beyond the scheduler itself.

### 3. Version compatibility

The bug exists from v5.3-rc1 to v5.17, which includes the v5.15+ range that kSTEP supports. Kernel version is NOT the barrier to reproduction.

### 4. Alternative reproduction methods

The bug can be reproduced on any physical machine with a cpufreq driver and the schedutil governor:

- **Intel laptops**: Use the `intel_pstate` driver in passive mode with schedutil, or `acpi-cpufreq` with schedutil.
- **ARM platforms**: Most ARM SoCs with a cpufreq driver (e.g., `cpufreq-dt`) and schedutil.
- **Test procedure**: (a) Ensure schedutil is the active governor. (b) Run a CPU-bound workload with uclamp_max cap: `uclampset -M 0 sysbench --test=cpu --threads=N run`. (c) Monitor CPU frequency via `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` or `turbostat`. (d) On buggy kernels, frequency stays at maximum despite uclamp_max=0. On fixed kernels, frequency drops to minimum.
- **Alternatively**, use a VM with `qemu-system-x86_64 -cpu host` and KVM on a physical machine that exposes cpufreq to the guest (this requires specific hypervisor support and is not universally available).
