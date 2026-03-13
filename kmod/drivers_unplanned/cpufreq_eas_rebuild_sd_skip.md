# Cpufreq: EAS sched-domain rebuild skipped on shared-tunables path

**Commit:** `70d8b6485b0bcd135b6699fc4252d2272818d1fb`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v6.13-rc1
**Buggy since:** v6.7-rc1 (introduced by `e7a1b32e43b1` "cpufreq: Rebuild sched-domains when removing cpufreq driver")

## Bug Description

The schedutil cpufreq governor has an initialization function `sugov_init()` that is called once for each cpufreq policy. On systems where `CPUFREQ_HAVE_GOVERNOR_PER_POLICY` is **not** set (e.g., ACPI/CPPC-based platforms), a single set of global tunables is shared across all policies. In `sugov_init()`, there are two success paths depending on whether `global_tunables` has already been allocated by a previous policy's initialization:

1. **First policy (new tunables):** Allocates tunables, calls `kobject_init_and_add()`, then falls through to `sugov_eas_rebuild_sd()` and the `out:` label.
2. **Subsequent policies (shared tunables):** Finds `global_tunables` already set, hooks into the existing attribute set, and jumps directly to the `out:` label via `goto out`.

The bug is that `sugov_eas_rebuild_sd()` was placed **before** the `out:` label, meaning it was only called on the first path (new tunables allocation). When subsequent policies took the `goto out` path, `sugov_eas_rebuild_sd()` was skipped entirely.

This is significant because the Energy Aware Scheduler (EAS) requires **all** cpufreq policies to be governed by schedutil before it can be enabled. During boot, policies are initialized sequentially. The first policy triggers `sugov_eas_rebuild_sd()`, but at that point not all policies are under schedutil yet, so EAS remains disabled. When the final policy initializes (taking the `goto out` path), the sched-domain rebuild that would enable EAS is never triggered, leaving EAS permanently disabled on the system.

## Root Cause

In `sugov_init()`, the function has a conditional branch based on whether `global_tunables` is already set:

```c
if (global_tunables) {
    if (WARN_ON(have_governor_per_policy())) {
        ret = -EINVAL;
        goto stop_kthread;
    }
    policy->governor_data = sg_policy;
    sg_policy->tunables = global_tunables;
    gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
    goto out;  // <-- skips sugov_eas_rebuild_sd()
}

// ... allocate new tunables, kobject_init_and_add() ...

sugov_eas_rebuild_sd();  // <-- only reached on first path

out:
    mutex_unlock(&global_tunables_lock);
    return 0;
```

The `sugov_eas_rebuild_sd()` call was placed between the `kobject_init_and_add()` success and the `out:` label. When the `global_tunables` branch takes `goto out`, it jumps past this call entirely.

The function `sugov_eas_rebuild_sd()` schedules `rebuild_sd_work` which calls `rebuild_sched_domains_energy()`. This function rebuilds the scheduling domains and re-evaluates whether EAS should be enabled. EAS enablement depends on `sched_energy_enabled()` which checks (among other things) that all cpufreq policies use the schedutil governor. Without the sched-domain rebuild after the **last** policy is initialized under schedutil, the kernel never learns that all policies now satisfy the EAS requirement.

The bug was introduced in commit `e7a1b32e43b1`, which moved the sched-domain rebuild call from the cpufreq core (`sched_cpufreq_governor_change()`) into `sugov_init()` and `sugov_exit()` directly. When placing the call in `sugov_init()`, the author put it only on the new-tunables path, not accounting for the shared-tunables `goto out` path.

## Consequence

The observable impact is that **Energy Aware Scheduling (EAS) is not enabled at boot** on systems where it should be. This was confirmed by Pierre Gondois on an ACPI/CPPC-based ARM platform. EAS is the primary mechanism for energy-efficient task placement on heterogeneous (big.LITTLE / DynamIQ) ARM systems. Without EAS:

- The scheduler falls back to standard load-balancing heuristics that do not consider CPU energy efficiency.
- Tasks may be placed on high-performance (big) cores unnecessarily, leading to **increased power consumption** and **reduced battery life** on mobile/embedded devices.
- The performance-per-watt characteristic of heterogeneous systems is significantly degraded.

This is not a crash or data corruption bug — it is a silent performance/energy regression. The system continues to function correctly from a scheduling perspective, but EAS-dependent energy optimizations are absent. The issue specifically affects platforms without `CPUFREQ_HAVE_GOVERNOR_PER_POLICY` set, which includes ACPI/CPPC-based systems. Platforms with per-policy governors (like some DT-based systems) are not affected because each policy initialization goes through the new-tunables path and calls `sugov_eas_rebuild_sd()`.

## Fix Summary

The fix is a simple two-line change that moves `sugov_eas_rebuild_sd()` from before the `out:` label to after it:

```c
// Before fix:
    sugov_eas_rebuild_sd();  // only reached via new-tunables path
out:
    mutex_unlock(&global_tunables_lock);

// After fix:
out:
    sugov_eas_rebuild_sd();  // reached by BOTH success paths
    mutex_unlock(&global_tunables_lock);
```

By placing `sugov_eas_rebuild_sd()` after the `out:` label, it is called on **every** successful initialization — both when new tunables are allocated (first policy) and when global tunables are reused (subsequent policies). This ensures that every time a policy is successfully brought under schedutil governance, a sched-domain rebuild is triggered.

The fix is correct and complete because `sugov_eas_rebuild_sd()` merely schedules a work item (`schedule_work(&rebuild_sd_work)`) which asynchronously calls `rebuild_sched_domains_energy()`. This is idempotent and safe to call multiple times — the sched-domain rebuild simply re-evaluates EAS conditions each time. The important thing is that the **last** policy initialization triggers it, at which point all policies are under schedutil and EAS can be enabled.

## Triggering Conditions

The bug requires the following precise conditions:

- **Kernel version:** v6.7-rc1 through v6.12-rc6 (commits `e7a1b32e43b1` through `70d8b6485b0bcd135b6699fc4252d2272818d1fb~1`).
- **CONFIG_ENERGY_MODEL=y:** EAS support must be compiled in (otherwise `sugov_eas_rebuild_sd()` is a no-op inline stub).
- **CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y** or schedutil set as the governor: The governor must be schedutil for EAS to be considered.
- **`CPUFREQ_HAVE_GOVERNOR_PER_POLICY` NOT set:** The cpufreq driver must use shared (global) tunables. This is the case for ACPI/CPPC-based cpufreq drivers (e.g., `cppc_cpufreq`). DT-based drivers with per-policy governors are NOT affected.
- **Multiple cpufreq policies:** The system must have at least two cpufreq policies. The first policy's `sugov_init()` correctly calls `sugov_eas_rebuild_sd()` (but EAS cannot enable yet since not all policies are under schedutil). The second (and subsequent) policy's `sugov_init()` takes the `goto out` path, skipping the rebuild.
- **Heterogeneous CPU topology (big.LITTLE/DynamIQ):** EAS is only relevant on systems with asymmetric CPU capacities where energy-aware task placement makes a difference.

This is a **deterministic** bug — it occurs on every boot of an affected system configuration. There is no race condition or timing dependency; the control flow deterministically skips the function call.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. The reasons are:

1. **No cpufreq driver or governor lifecycle:** The bug is in the initialization path of the schedutil cpufreq governor (`sugov_init()`). kSTEP does not have a cpufreq driver registered in QEMU — there is no real frequency scaling hardware. While kSTEP provides `kstep_cpu_set_freq()` to set CPU frequency scaling factors directly, this bypasses the entire cpufreq subsystem (driver → governor → policy lifecycle). The `sugov_init()` function is never called because there is no cpufreq driver to trigger governor initialization.

2. **No cpufreq policy management:** The bug requires multiple cpufreq policies being initialized sequentially, with the global tunables being shared across policies. kSTEP has no mechanism to create, initialize, or manage cpufreq policies. The `cpufreq_policy` structure and its lifecycle (registration, governor attachment, policy start/stop) are entirely managed by the cpufreq core in response to real hardware driver events.

3. **No EAS enablement check infrastructure:** Reproducing this bug requires verifying that EAS is (not) enabled after all policies are initialized. The EAS enablement check happens inside `rebuild_sched_domains_energy()` which evaluates `sched_energy_enabled()`. Even if we could trigger the governor init, the EAS check depends on energy model data (`em_perf_domain`) being registered for the CPUs, which requires a real cpufreq driver with energy model support.

4. **QEMU has no frequency scaling hardware:** QEMU's virtual CPUs do not expose cpufreq interfaces. There is no ACPI CPPC, no DT-based OPP table, and no cpufreq driver loaded. Without a cpufreq driver, the schedutil governor is never initialized, and the buggy code path is never executed.

**What would need to change in kSTEP to support this:**
- A full cpufreq driver emulation layer would be needed: a fake cpufreq driver that registers policies, supports governor attachment, and provides energy model data. This is fundamentally outside kSTEP's architecture, which focuses on scheduler behavior testing rather than cpufreq subsystem testing.
- Alternatively, kSTEP would need to directly call `sugov_init()` with crafted `cpufreq_policy` structures, but this requires extensive subsystem initialization (cpufreq core, governor infrastructure, sysfs kobjects) that cannot be reasonably faked.

**Alternative reproduction methods:**
- Use a real ARM platform with ACPI/CPPC cpufreq (e.g., Ampere Altra, or ARM Juno with appropriate firmware) and check `/proc/sys/kernel/sched_energy_aware` or trace `rebuild_sched_domains_energy()` during boot.
- Use a virtual machine with a paravirtualized cpufreq driver (if available) that does not set `CPUFREQ_HAVE_GOVERNOR_PER_POLICY`.
- Add `pr_info()` traces to `sugov_init()` to confirm which path is taken and whether `sugov_eas_rebuild_sd()` is called, then boot on an affected platform.
- Pierre Gondois confirmed reproduction on an ACPI/CPPC-based platform in the mailing list thread.
