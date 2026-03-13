# Cpufreq: Schedutil Skips HWP Limit Updates When Target Frequency Unchanged

**Commit:** `d1e7c2996e988866e7ceceb4641a0886885b7889`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v5.10-rc2
**Buggy since:** v5.9-rc1 (introduced by commit `f6ebbcf08f37` "cpufreq: intel_pstate: Implement passive mode with HWP enabled")

## Bug Description

The schedutil cpufreq governor contains two frequency-caching optimizations that skip calling the cpufreq driver when the computed target frequency has not changed. These optimizations are correct for traditional cpufreq drivers where the only goal is to set a target frequency. However, they are incorrect for drivers like `intel_pstate` in passive mode with Hardware P-states (HWP) enabled, because such drivers need to update internal upper and lower frequency boundaries (HWP min and HWP max limits) whenever the cpufreq policy limits change, even if the target frequency itself remains unchanged.

The bug manifests when the `intel_pstate` driver operates in passive mode with HWP enabled. In this mode, the driver needs its `->target()` callback invoked whenever `policy->min` or `policy->max` changes so it can program the corresponding HWP request MSR fields. The schedutil governor, however, has two caching layers that can suppress the driver callback:

1. `sugov_update_next_freq()` compares the newly computed `next_freq` against `sg_policy->next_freq` and returns `false` (skip) if they are equal.
2. `get_next_freq()` compares the raw mapped frequency against `sg_policy->cached_raw_freq` and returns the cached `sg_policy->next_freq` without calling `cpufreq_driver_resolve_freq()` if the raw frequency is unchanged and `need_freq_update` is not set.

A concrete example of the failure: if the policy min is set to some value X (causing the target frequency to settle at X), and then the policy max is later set to the same value X, the HWP max limit should be updated to X. But because the target frequency remains X (unchanged), both caching layers suppress the driver callback and the HWP max register is never updated. This leaves a stale, potentially higher HWP max limit in the hardware.

## Root Cause

The root cause is that the two frequency-caching optimizations in schedutil were designed with the assumption that the only purpose of calling the cpufreq driver is to set a new target frequency. This assumption breaks when a driver uses the `CPUFREQ_NEED_UPDATE_LIMITS` flag (introduced in prerequisite commit `1c534352f47f`) to indicate it needs callbacks for policy limit changes independent of frequency changes.

**First caching layer — `sugov_update_next_freq()`:**

```c
static bool sugov_update_next_freq(struct sugov_policy *sg_policy, u64 time,
                                   unsigned int next_freq)
{
    if (sg_policy->next_freq == next_freq)
        return false;  /* BUG: skips even when limits changed */
    ...
}
```

This function is called by both `sugov_fast_switch()` and `sugov_deferred_update()`. When it returns `false`, neither `cpufreq_driver_fast_switch()` nor the irq_work path is invoked, meaning the driver never sees the new policy limits.

**Second caching layer — `get_next_freq()`:**

```c
static unsigned int get_next_freq(struct sugov_policy *sg_policy,
                                  unsigned long util, unsigned long max)
{
    ...
    freq = map_util_freq(util, freq, max);

    if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
        return sg_policy->next_freq;  /* BUG: returns cached freq, skips resolve */
    ...
}
```

Even when `sugov_should_update_freq()` correctly detects a limits change and sets `need_freq_update = true`, this second layer can still short-circuit if the raw frequency happens to match the cache. The `need_freq_update` flag does guard against this, but it only fires once — if the frequency resolve returns the same value as before, `sugov_update_next_freq()` still blocks the actual driver call.

The interaction between these two layers creates a scenario where policy limit changes are silently dropped. The `limits_changed` flag in `sugov_should_update_freq()` does set `need_freq_update = true`, which bypasses the second layer's cache check in `get_next_freq()`. However, `cpufreq_driver_resolve_freq()` may resolve to the same frequency as `sg_policy->next_freq`, causing `sugov_update_next_freq()` to return `false` and suppress the driver callback entirely. The net result is that the driver's `->target()` or `->fast_switch()` callback is never invoked despite the policy limits having changed.

## Consequence

The observable impact is that Hardware P-state (HWP) min and max limits become stale and desynchronized from the cpufreq policy limits. This has several practical consequences:

1. **HWP max limit too high:** If the policy max is reduced (e.g., by thermal throttling or administrative action) but the target frequency is already at or below the new max, the HWP max register retains its old higher value. The CPU hardware may boost above the intended maximum, consuming more power than desired and potentially exceeding thermal limits.

2. **HWP min limit too low:** Conversely, if the policy min is raised but the target frequency is already at or above the new min, the HWP min register may retain its old lower value. The CPU hardware may drop below the intended minimum performance floor, causing latency-sensitive workloads to experience unexpected slowdowns.

3. **Policy limits become effectively ignored:** Any sequence of policy limit changes that does not also change the schedutil target frequency will be silently dropped. This breaks the fundamental contract between the cpufreq core and the driver: that policy limits are authoritative and always respected.

The bug was reported by Zhang Rui from Intel, who observed that HWP limits were not being updated correctly when policy limits changed without a corresponding target frequency change. This is particularly problematic because the entire purpose of the HWP passive mode (introduced in the bug-introducing commit `f6ebbcf08f37`) was to allow the CPU scheduler and HWP algorithm to cooperate, with the scheduler setting meaningful floor and ceiling values.

## Fix Summary

The fix adds a check for the `CPUFREQ_NEED_UPDATE_LIMITS` driver flag at both caching layers, ensuring that drivers requiring limit updates always have their callbacks invoked:

**In `sugov_update_next_freq()`:**

```c
if (sg_policy->next_freq == next_freq &&
    !cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS))
    return false;
```

When `CPUFREQ_NEED_UPDATE_LIMITS` is set, the function no longer returns `false` on equal frequencies, allowing the driver callback to proceed unconditionally.

**In `get_next_freq()`:**

```c
if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update &&
    !cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS))
    return sg_policy->next_freq;
```

Similarly, when `CPUFREQ_NEED_UPDATE_LIMITS` is set, the raw frequency cache is bypassed, forcing `cpufreq_driver_resolve_freq()` to be called. This ensures the driver always receives the current policy limits.

The fix is correct and complete because it addresses both layers of caching that can suppress driver callbacks. The `cpufreq_driver_test_flags()` helper (introduced in prerequisite commit `a62f68f5ca53`) provides a clean API to check driver flags from the schedutil code. The fix is also backward-compatible: for drivers that do not set `CPUFREQ_NEED_UPDATE_LIMITS`, the caching behavior remains exactly as before, preserving the existing performance optimization for the common case.

## Triggering Conditions

To trigger this bug, the following conditions must all be met:

1. **Hardware:** An Intel CPU with Hardware P-state (HWP) support. The CPU must support the `MSR_HWP_REQUEST` register (Intel Skylake or newer).

2. **Driver:** The `intel_pstate` cpufreq driver must be loaded in passive mode with HWP enabled. This requires booting with `intel_pstate=passive` and the platform supporting HWP (which is auto-detected).

3. **Governor:** The schedutil cpufreq governor must be active (the default when intel_pstate is in passive mode).

4. **Policy limit sequence:** The cpufreq policy limits must change in a specific order:
   - First, set `policy->min` to a value X, causing the schedutil target frequency to settle at X (because util is low enough that `map_util_freq()` computes X or lower).
   - Then, set `policy->max` to the same value X. Since the target frequency is already X, neither caching layer triggers a driver callback, and the HWP max register is not updated.

5. **Utilization:** CPU utilization must be low enough that the schedutil-computed target frequency matches the policy min, so that changing policy max does not change the target frequency.

The bug is deterministic given the above conditions — it is not a race condition. Any policy limit change that does not also change the schedutil target frequency will fail to propagate to the HWP hardware. The probability of triggering this is moderate to high on affected systems, since thermal management, power capping, and administrative frequency limits all change policy bounds without necessarily changing the current target.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for two reasons:

### 1. Kernel Version Too Old

The fix was merged in v5.10-rc2 and the bug existed only from v5.9-rc1 through v5.10-rc1. kSTEP supports Linux v5.15 and newer only. Any kernel version that kSTEP can build and run already contains this fix, so the buggy code path cannot be reached.

### 2. No cpufreq Hardware or Driver in QEMU

Even if the kernel version constraint were lifted, kSTEP runs inside QEMU which does not emulate cpufreq hardware. Specifically:

- **No HWP MSRs:** QEMU's x86 CPU model does not emulate `MSR_HWP_REQUEST`, `MSR_HWP_CAPABILITIES`, or any of the Hardware P-state MSR registers. Without these, the `intel_pstate` driver will not load in HWP mode.

- **No cpufreq driver registration:** Without a cpufreq driver registered in the kernel, the schedutil governor is never activated. The `cpufreq_driver` global pointer remains NULL, meaning `cpufreq_driver_test_flags()` cannot even be called (it assumes a driver is present).

- **No frequency scaling infrastructure:** kSTEP's `kstep_cpu_set_freq()` sets the `arch_freq_scale` per-CPU variable to influence PELT calculations, but it does not register a cpufreq driver, create cpufreq policy objects, or activate the schedutil governor. The entire `cpufreq_schedutil.c` code path is inert.

- **`sugov_update_next_freq()` and `get_next_freq()` are static functions** within `cpufreq_schedutil.c` and are only called from the schedutil governor's `sugov_update_shared()` and `sugov_update_single()` hooks, which are registered as cpufreq utilization update callbacks. Without a cpufreq policy, these callbacks are never registered and never invoked.

### 3. What Would Be Needed

To reproduce this in kSTEP, the following fundamental changes would be required:

- **A mock cpufreq driver** that registers with the cpufreq core, creates policy objects, and sets the `CPUFREQ_NEED_UPDATE_LIMITS` flag. This driver would need to implement `->init()`, `->verify()`, and either `->target()` or `->fast_switch()` callbacks.
- **Schedutil governor activation** triggered by the mock driver's policy creation.
- **Policy limit manipulation APIs** (e.g., `kstep_cpufreq_set_policy_min/max()`) to trigger the `limits_changed` flag in schedutil.
- **HWP register emulation** or at minimum a way to observe whether the driver's `->target()`/`->fast_switch()` callback was invoked with the correct parameters.

This represents a fundamental expansion of kSTEP's architecture well beyond its current scope — it would essentially require building a cpufreq subsystem emulation layer.

### 4. Alternative Reproduction Methods

The most practical reproduction approach outside kSTEP:

1. Use a bare-metal Intel system with HWP support (Skylake or newer).
2. Boot a v5.9.x or v5.10-rc1 kernel with `intel_pstate=passive`.
3. Set a low CPU utilization workload (e.g., idle or very light load).
4. Write to `/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq` to set the min to some value X.
5. Wait for the schedutil target to settle at X.
6. Write to `/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq` to set max to X.
7. Read the HWP request MSR (`rdmsr 0x774`) and verify the max field was NOT updated (bug present) or WAS updated (fix applied).
