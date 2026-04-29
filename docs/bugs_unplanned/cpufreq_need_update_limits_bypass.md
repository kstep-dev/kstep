# Cpufreq: Schedutil Skips Policy Limits Update for Non-CPUFREQ_NEED_UPDATE_LIMITS Drivers

**Commit:** `cfde542df7dd51d26cf667f4af497878ddffd85a`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v6.15-rc3
**Buggy since:** v6.14-rc1 (introduced by commit `8e461a1cb43d` "cpufreq: schedutil: Fix superfluous updates caused by need_freq_update")

## Bug Description

The schedutil cpufreq governor fails to apply updated cpufreq policy limits (such as those imposed by thermal throttling) for cpufreq drivers that support fast frequency switching but do not set the `CPUFREQ_NEED_UPDATE_LIMITS` flag. This effectively disables thermal frequency capping for a large class of cpufreq drivers when the schedutil governor is in use, causing CPUs to run at frequencies exceeding the thermal limits.

The bug was introduced by commit `8e461a1cb43d` which intended to reduce superfluous frequency updates for drivers with `CPUFREQ_NEED_UPDATE_LIMITS`. That commit moved the `CPUFREQ_NEED_UPDATE_LIMITS` test from `sugov_update_next_freq()` into `sugov_should_update_freq()`, so that `need_freq_update` was only set to `true` when `limits_changed` was set AND the driver had `CPUFREQ_NEED_UPDATE_LIMITS`. For all other drivers, `need_freq_update` was set to `false` when policy limits changed.

The consequence is that for drivers without `CPUFREQ_NEED_UPDATE_LIMITS` (which includes scmi-cpufreq, cpufreq-dt, mediatek, qcom, and many other ARM/embedded drivers), the `get_next_freq()` function's early-return optimization kicks in: when the raw computed frequency hasn't changed (because utilization is still high), `get_next_freq()` returns the cached `next_freq` without calling `cpufreq_driver_resolve_freq()`. Since `cpufreq_driver_resolve_freq()` is the function that clamps the frequency to the current `policy->min` and `policy->max` bounds, the new lower maximum from thermal throttling is never applied.

## Root Cause

The root cause lies in the interaction between three functions in `kernel/sched/cpufreq_schedutil.c`:

**1. `sugov_should_update_freq()` (the limits_changed handler):**
In the buggy code (after commit `8e461a1cb43d`), when `sg_policy->limits_changed` is `true`, the function sets:
```c
sg_policy->need_freq_update = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);
```
For drivers without `CPUFREQ_NEED_UPDATE_LIMITS`, this sets `need_freq_update = false`. The function still returns `true` (allowing the frequency update to proceed), but the critical `need_freq_update` flag remains unset.

**2. `get_next_freq()` (the frequency computation function):**
This function computes the raw desired frequency from CPU utilization, then checks:
```c
if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update)
    return sg_policy->next_freq;
```
When the CPU is under sustained high load (e.g., a stress test), the raw frequency computed from utilization stays at the maximum. Since `cached_raw_freq` hasn't changed and `need_freq_update` is `false`, the function returns the previously cached `next_freq` (which is the un-throttled maximum frequency) **without** calling `cpufreq_driver_resolve_freq()`. This means the new `policy->max` limit (lowered by thermal cooling) is never applied to the returned frequency.

**3. `sugov_update_next_freq()` (the frequency commit gate):**
Even if `get_next_freq()` had returned a different frequency, there is a second optimization in `sugov_update_next_freq()`:
```c
if (sg_policy->need_freq_update)
    sg_policy->need_freq_update = false;
else if (sg_policy->next_freq == next_freq)
    return false;
```
Without `need_freq_update` set, if the computed next frequency equals the stored `next_freq`, the function returns `false` and skips invoking the driver callback entirely. This is the intended optimization for avoiding redundant driver calls, but it becomes harmful when policy limits have changed because the "same frequency" may now exceed the allowed maximum.

The fundamental error in commit `8e461a1cb43d` was conflating two distinct purposes of `need_freq_update`: (a) forcing `get_next_freq()` to re-resolve the frequency through `cpufreq_driver_resolve_freq()` (needed for ALL drivers when policy limits change, so the new min/max bounds are applied), and (b) forcing the driver callback to be invoked even when the resolved frequency hasn't changed (only needed for `CPUFREQ_NEED_UPDATE_LIMITS` drivers). By gating `need_freq_update` on `CPUFREQ_NEED_UPDATE_LIMITS` at the point of `limits_changed`, both purposes were disabled for non-`CPUFREQ_NEED_UPDATE_LIMITS` drivers.

## Consequence

The most significant impact is that **thermal throttling via cpufreq cooling is completely ineffective** when the schedutil governor is active on affected platforms. Specifically:

When a thermal zone reaches a trip point and the kernel's thermal framework lowers `policy->max` to throttle CPU frequency, the schedutil governor ignores this new limit and continues requesting the old (higher) frequency. The CPU continues operating at the un-throttled maximum frequency for as long as the workload keeps utilization high. The thermal limit is only applied when the workload stops (causing utilization to drop, which changes the raw computed frequency and forces a re-resolution). This means the device can overheat beyond its designed thermal limits, potentially causing hardware damage, thermal shutdown, or degraded component longevity.

Stephan Gerhold reported this regression on a Qualcomm X1E laptop using scmi-cpufreq. He demonstrated that with the "performance" governor (which doesn't use schedutil's optimization path), thermal throttling worked correctly. With "schedutil", CPU frequencies stayed at maximum regardless of temperature, and throttling only took effect after stopping the stress test. The bug affects ALL cpufreq drivers that set `fast_switch_possible` but do NOT set `CPUFREQ_NEED_UPDATE_LIMITS`, which includes the majority of ARM/embedded cpufreq drivers (scmi-cpufreq, cpufreq-dt, mediatek-cpufreq, qcom-cpufreq, etc.). Only intel-pstate and amd-pstate (sometimes) set `CPUFREQ_NEED_UPDATE_LIMITS`.

## Fix Summary

The fix by Rafael J. Wysocki restructures the `need_freq_update` logic to correctly separate the two purposes of the flag:

**In `sugov_should_update_freq()`:** The fix restores the original behavior of unconditionally setting `need_freq_update = true` when `limits_changed` is set:
```c
sg_policy->need_freq_update = true;  // was: cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS)
```
This ensures that `get_next_freq()` will always call `cpufreq_driver_resolve_freq()` after a policy limits change, re-clamping the frequency to the new `policy->min`/`policy->max` bounds regardless of whether the raw utilization-based frequency has changed.

**In `sugov_update_next_freq()`:** The fix moves the `CPUFREQ_NEED_UPDATE_LIMITS` check to this function, where it serves its intended purpose of deciding whether to invoke the driver callback when the resolved frequency hasn't changed:
```c
if (sg_policy->need_freq_update) {
    sg_policy->need_freq_update = false;
    if (sg_policy->next_freq == next_freq &&
        !cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS))
        return false;
}
```
This means: after a limits change, if the newly resolved frequency (after applying new limits) equals the previously selected frequency, skip the driver callback UNLESS the driver has `CPUFREQ_NEED_UPDATE_LIMITS` (in which case the driver wants to be notified of every limits change). This preserves the original optimization from commit `8e461a1cb43d` — avoiding superfluous driver callbacks — while ensuring that the frequency is always properly re-resolved through `cpufreq_driver_resolve_freq()` when limits change.

## Triggering Conditions

The following conditions must all be met to trigger this bug:

1. **Kernel version:** v6.14-rc1 through v6.15-rc2 (any kernel containing commit `8e461a1cb43d` but not the fix `cfde542df7dd`).

2. **Cpufreq driver:** A cpufreq driver must be loaded that has `fast_switch_possible` or `fast_switch_enabled` set to `true`, but does NOT set the `CPUFREQ_NEED_UPDATE_LIMITS` driver flag. This includes scmi-cpufreq, cpufreq-dt, and most ARM/embedded cpufreq drivers.

3. **Governor:** The schedutil governor must be active (`scaling_governor` set to `schedutil`). Other governors (performance, powersave, ondemand, conservative) are not affected because they do not use the `sugov_should_update_freq()` / `get_next_freq()` / `sugov_update_next_freq()` path.

4. **Policy limits change:** Something must change `policy->max` or `policy->min` at runtime. The most common trigger is thermal throttling (a thermal zone trip point triggers cpufreq cooling, which lowers `policy->max`). It can also be triggered by writing to `/sys/devices/system/cpu/cpufreq/policyN/scaling_max_freq` via sysfs.

5. **Sustained high utilization:** The workload must keep CPU utilization high enough that the raw frequency computed by `get_next_freq()` (before policy clamping) remains unchanged at the maximum. If utilization drops (causing the raw frequency to decrease), the bug is masked because the changed raw frequency triggers re-resolution via `cpufreq_driver_resolve_freq()`.

The bug is 100% reproducible when these conditions are met. There is no race condition or timing dependency — it is a deterministic logic error in the code path. The frequency simply stays at the old (un-throttled) value until the workload changes enough to alter the raw computed frequency.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP in its current form.

**1. Why this bug cannot be reproduced with kSTEP:**

The bug is entirely within the cpufreq schedutil governor subsystem (`kernel/sched/cpufreq_schedutil.c`), specifically in the interaction between cpufreq policy limits changes and the schedutil governor's frequency update decision logic. Reproducing it requires:

- **A registered cpufreq driver:** The schedutil governor is only active when a cpufreq driver is registered and a cpufreq policy exists. QEMU's virtual CPUs do not have a cpufreq driver — there is no frequency scaling hardware to manage. Without a cpufreq driver, there is no cpufreq policy, no governor, and the entire `sugov_*` code path is dead code.

- **Policy limits manipulation:** The bug is triggered by changing `policy->max` (or `policy->min`) at runtime. This happens through the cpufreq cooling device (`freq_qos_update_request()`) or sysfs writes to `scaling_max_freq`. Both mechanisms require an active cpufreq policy backed by a real or dummy cpufreq driver.

- **Frequency resolution observation:** To verify whether the bug is triggered, one must observe whether `cpufreq_driver_resolve_freq()` is called (or skipped) and whether the driver's `target()` / `fast_switch()` callback is invoked with the correct (clamped) frequency. This requires instrumentation of the cpufreq subsystem internals.

**2. What would need to be added to kSTEP to support this:**

Reproducing this bug would require a **fundamentally new subsystem** in kSTEP:

- **Dummy cpufreq driver registration:** kSTEP would need the ability to register a minimal cpufreq driver (implementing `cpufreq_driver` with `init`, `verify`, `target`/`fast_switch`, and a frequency table) from the kernel module. This driver would NOT set `CPUFREQ_NEED_UPDATE_LIMITS`. This is not a minor helper function — it involves integrating with the entire cpufreq core subsystem (`cpufreq_register_driver()`, policy creation, governor selection, etc.).

- **Governor activation:** After registering the driver, the schedutil governor would need to be attached to the policy. This typically happens automatically via `cpufreq_init_policy()`, but verifying and controlling this from a test driver adds complexity.

- **Policy limits manipulation API:** A new kSTEP API like `kstep_cpufreq_set_max(cpu, freq)` would be needed to write to `scaling_max_freq` or invoke `freq_qos_update_request()` to change the policy limits at runtime.

- **Schedutil state observation:** New observation hooks would be needed to read `sg_policy->need_freq_update`, `sg_policy->next_freq`, and the return value of `get_next_freq()` to verify whether the frequency was properly re-resolved after a limits change.

This is not a minor extension — it requires integrating kSTEP with the cpufreq subsystem, which is outside the scheduler core that kSTEP was designed to test.

**3. Alternative reproduction methods:**

The bug can be reproduced on any ARM/embedded platform with a cpufreq driver that supports fast switching but doesn't set `CPUFREQ_NEED_UPDATE_LIMITS`:

- Boot a kernel with the buggy commit (`8e461a1cb43d`) applied.
- Set the schedutil governor: `echo schedutil > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor`
- Start a CPU stress test: `stress-ng --cpu $(nproc) --cpu-method matrixprod`
- Lower the policy max: `echo 1000000 > /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq`
- Observe that the actual CPU frequency (via `cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq`) remains at the old maximum, ignoring the new limit.
- Stop the stress test and observe that the frequency finally drops.

Alternatively, configure thermal trip points at low temperatures to trigger cpufreq cooling, as Stephan Gerhold did in his report.
