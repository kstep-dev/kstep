# Cpufreq: Missing Memory Barriers for schedutil limits_changed Flag

**Commit:** `79443a7e9da3c9f68290a8653837e23aba0fa89f`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v6.15-rc3
**Buggy since:** v5.3-rc5 (commit `600f5badb78c` "cpufreq: schedutil: Don't skip freq update when limits change")

## Bug Description

The schedutil cpufreq governor uses a `limits_changed` flag in `struct sugov_policy` to communicate between the cpufreq core (which updates policy frequency limits) and the scheduler-driven frequency selection path. When cpufreq policy limits change (e.g., a user writes to `/sys/devices/system/cpu/cpufreq/policyN/scaling_max_freq`), the function `sugov_limits()` is called, which sets `sg_policy->limits_changed = true`. Later, when the scheduler's per-tick or utilization-update hook fires, `sugov_should_update_freq()` checks this flag, clears it, and forces a frequency recalculation that respects the new limits.

The bug is that the `limits_changed` flag is accessed from two different CPU contexts — `sugov_limits()` runs on the CPU that initiates the policy change, while `sugov_should_update_freq()` runs on the CPU whose frequency is being governed — without any memory barriers or compiler annotations (`READ_ONCE`/`WRITE_ONCE`). This means the CPU and compiler are free to reorder the flag accesses relative to the actual reads and writes of the policy limits (`policy->min`, `policy->max`).

There are two distinct reordering scenarios that can cause policy limit updates to be silently missed. In the first scenario, the clearing of `limits_changed` in `sugov_should_update_freq()` gets reordered after the reads of `policy->min`/`policy->max` in `cpufreq_driver_resolve_freq()`. In the second scenario, the setting of `limits_changed` in `sugov_limits()` gets reordered after the actual updates of `policy->min`/`policy->max` in `cpufreq_set_policy()`. Both scenarios can result in the governor operating with stale frequency limits, potentially capping or uncapping CPU frequency incorrectly for an extended period.

Additionally, without `READ_ONCE()`/`WRITE_ONCE()`, the compiler is free to merge, split, or reorder the loads and stores to the `limits_changed` flag, introducing further potential for inconsistency — particularly relevant since these accesses occur in hot paths that the compiler is likely to optimize aggressively.

## Root Cause

The root cause is the absence of explicit memory ordering between the `limits_changed` flag and the policy limits variables it guards. The flag is intended to act as a notification mechanism: "the limits have changed, please re-read them." For this to work correctly, two ordering constraints must hold:

**Scenario 1 — Reader side (`sugov_should_update_freq()`):** When this function reads `limits_changed == true` and clears it to `false`, the clearing must be visible to other CPUs *before* the subsequent reads of `policy->min` and `policy->max` in `cpufreq_driver_resolve_freq()`. Without this ordering, the following interleaving is possible:

1. CPU A (policy update): `sugov_limits()` sets `limits_changed = true`
2. CPU B (scheduler): `sugov_should_update_freq()` reads `limits_changed == true`
3. CPU B: Reads `policy->min` and `policy->max` (with OLD values — reordered before step 4)
4. CPU B: Writes `limits_changed = false` (reordered after step 3)
5. CPU A: `cpufreq_set_policy()` writes new `policy->min` and `policy->max`
6. CPU A: `sugov_limits()` sets `limits_changed = true` — but this gets CLOBBERED by step 4

The result is that CPU B's late write of `limits_changed = false` overwrites CPU A's new `limits_changed = true`, and the new policy limits are never picked up. The governor continues operating with the old limits indefinitely.

**Scenario 2 — Writer side (`sugov_limits()`):** The call chain in `cpufreq_set_policy()` calls `sugov_limits()` *before* updating `policy->min` and `policy->max`. However, without a write memory barrier, the store to `limits_changed = true` in `sugov_limits()` may be reordered *after* the stores to `policy->min`/`policy->max`. If `sugov_should_update_freq()` on another CPU executes between the policy limits update and the (reordered) `limits_changed` store, it will see `limits_changed == false` and use the rate-limit-based check, potentially skipping the frequency update entirely. By the time `limits_changed` becomes visible, there may be no further trigger to re-read the new limits.

The specific code before the fix in `sugov_should_update_freq()` was:
```c
if (unlikely(sg_policy->limits_changed)) {
    sg_policy->limits_changed = false;
    sg_policy->need_freq_update = true;
    return true;
}
```

And in `sugov_limits()`:
```c
sg_policy->limits_changed = true;
```

Neither location has any memory barriers, and neither uses `READ_ONCE()`/`WRITE_ONCE()` to prevent compiler-level reordering or optimization.

The `ignore_dl_rate_limit()` function also writes to `limits_changed` without `WRITE_ONCE()`:
```c
if (cpu_bw_dl(cpu_rq(sg_cpu->cpu)) > sg_cpu->bw_min)
    sg_cpu->sg_policy->limits_changed = true;
```

## Consequence

The observable impact is that cpufreq policy frequency limit changes can be silently lost for an unbounded period. Specifically:

When a user or system component changes the CPU frequency scaling limits (e.g., writing to `scaling_max_freq` or `scaling_min_freq`), the schedutil governor may fail to apply those new limits. This means a CPU could continue running at a frequency above the intended maximum or below the intended minimum. In the fast-switching case (where schedutil directly programs the hardware without a kernel thread), this is particularly severe because there is no other mechanism to force the new limits to take effect — the driver callback that actually applies frequency changes is only invoked when schedutil decides a frequency update is needed.

The practical consequences include: **performance degradation** when a minimum frequency cap is missed (the CPU stays at a lower frequency than required), **thermal issues or excessive power consumption** when a maximum frequency cap is missed (the CPU continues running at a higher frequency than allowed), and **violation of power management policies** when frequency limits set by thermal throttling, power budgeting, or userspace tools are ignored. The bug is described as "theoretical" in the commit message because it depends on specific memory reordering patterns, but on weakly-ordered architectures (like ARM) such reorderings are routine. On x86, the store-load reordering in Scenario 1 is also possible.

No crash, hang, or kernel oops results from this bug — it is a silent correctness issue where the system may operate outside its configured frequency bounds.

## Fix Summary

The fix adds explicit memory barriers and compiler annotations to ensure proper ordering between the `limits_changed` flag and the policy limits it guards.

In `sugov_should_update_freq()`, after clearing `limits_changed` to `false` via `WRITE_ONCE()`, an `smp_mb()` (full memory barrier) is inserted before returning `true`. This ensures the `limits_changed = false` store is globally visible before any subsequent reads of `policy->min`/`policy->max` in `cpufreq_driver_resolve_freq()`. This prevents Scenario 1 where a late `false` write could clobber a new `true` from `sugov_limits()`.

In `sugov_limits()`, an `smp_wmb()` (write memory barrier) is inserted *before* the `WRITE_ONCE(sg_policy->limits_changed, true)`. Since `sugov_limits()` is called from `cpufreq_set_policy()` before the policy limits are actually written, this write barrier ensures the `limits_changed = true` store cannot be reordered past the point where `cpufreq_set_policy()` updates `policy->min` and `policy->max`. This prevents Scenario 2 where the flag set could be reordered after the limits update and missed entirely.

All accesses to `limits_changed` are wrapped in `READ_ONCE()`/`WRITE_ONCE()` to prevent the compiler from merging, splitting, or reordering these accesses. This applies to the read in `sugov_should_update_freq()`, the writes in `sugov_should_update_freq()`, `sugov_limits()`, and `ignore_dl_rate_limit()`. The barrier pair (`smp_wmb` in writer paired with `smp_mb` in reader) is the standard Linux kernel pattern for this type of flag-guarded data publishing.

## Triggering Conditions

To trigger this bug, the following conditions must all be met:

1. **Schedutil governor active**: The system must be using the `schedutil` cpufreq governor (not `performance`, `powersave`, `ondemand`, etc.). This is the default on many modern Linux distributions.

2. **Multi-CPU system**: The bug requires concurrent execution on at least two CPUs. One CPU runs `sugov_limits()` (via `cpufreq_set_policy()`) while another CPU runs `sugov_should_update_freq()` (via the scheduler's `cpufreq_update_util()` hook during tick or task wakeup).

3. **Policy limits change during active scheduling**: A cpufreq policy limits change (e.g., writing to `scaling_max_freq`, `scaling_min_freq`, or thermal/power capping triggers) must occur while the scheduler is actively making frequency decisions on the governed CPUs.

4. **CPU memory reordering**: The bug manifests when the CPU reorders stores and loads across the `limits_changed` flag boundary. On weakly-ordered architectures (ARM, ARM64, POWER), this is common. On x86/x86_64 (TSO model), store-load reordering can still trigger Scenario 1. The compiler may also introduce reordering on any architecture.

5. **Timing window**: The race window is between `sugov_limits()` setting the flag and `sugov_should_update_freq()` clearing it, overlapping with the actual policy limits update in `cpufreq_set_policy()`. The window is narrow but can be hit under load, especially on fast-switching platforms where the scheduler tick path and the sysfs write path are both hot.

6. **Fast-switching preferred**: While the bug exists for both fast-switch and non-fast-switch paths, it is most impactful with fast switching because in that mode, running the driver callback is the *only* way to apply new frequency limits. In the non-fast-switch (kthread) path, `sugov_limits()` itself calls `cpufreq_policy_apply_limits()` which provides some mitigation.

The probability of reproduction depends on the architecture's memory ordering model and the workload's concurrency characteristics. On ARM64, it could happen with moderate regularity during concurrent limit changes and scheduling activity. On x86, it would be rarer due to the stronger memory model but is still theoretically possible.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

### 1. No cpufreq driver in QEMU

kSTEP runs kernels inside QEMU, which does not provide any CPU frequency scaling hardware or cpufreq driver. Without a registered cpufreq driver, the schedutil governor is never initialized, `sugov_limits()` is never called, and the `limits_changed` flag is never exercised. The `kstep_cpu_set_freq()` API manipulates `arch_set_freq_scale()` (the scheduler's frequency-invariant scaling factor), but this does **not** go through the cpufreq subsystem — it does not invoke `cpufreq_set_policy()`, does not call `sugov_limits()`, and does not update `policy->min`/`policy->max`.

### 2. Memory reordering cannot be reproduced in emulation

The core of this bug is CPU-level memory reordering between stores and loads on different CPUs. QEMU's TCG (Tiny Code Generator) software emulation provides sequentially consistent memory ordering, meaning stores and loads are always observed in program order. Even with QEMU/KVM (hardware virtualization), the memory ordering is governed by the host CPU's model, and the specific reordering window needed is essentially impossible to trigger deterministically in a virtualized environment. Memory barriers are NOPs when memory is already sequentially consistent.

### 3. What would need to change to support this

To reproduce this bug in kSTEP, the following **fundamental** changes would be required:

- **A cpufreq driver for QEMU**: A virtual cpufreq driver would need to be created that registers with the cpufreq subsystem, supports the schedutil governor, and allows policy limit changes. This is a significant infrastructure addition — it requires implementing a cpufreq driver with `frequency_table`, `target()` or `fast_switch()` callbacks, and sysfs integration.

- **A mechanism to trigger policy limit changes**: kSTEP would need a way to call `cpufreq_set_policy()` from the kernel module. This could potentially be done by writing to cpufreq sysfs files from kernel context (possible via `kernel_write()`), but it requires the cpufreq driver to exist first.

- **Real weakly-ordered hardware or memory reordering injection**: Even with the above, reproducing the actual reordering would require either running on real weakly-ordered hardware (ARM64) or implementing a memory reordering injection framework (e.g., selectively delaying store buffers). Neither is feasible within kSTEP's architecture.

### 4. Alternative reproduction methods

The most practical reproduction approaches outside kSTEP are:

- **Real ARM64 hardware**: Run a stress test that concurrently changes cpufreq policy limits (via sysfs writes in a loop) while running CPU-intensive workloads. Monitor the actual CPU frequency to detect cases where it violates the configured limits. Tools like `turbostat` or `perf` frequency monitoring can help.

- **KCSAN (Kernel Concurrency Sanitizer)**: Enable `CONFIG_KCSAN` in the kernel build. KCSAN can detect data races on the `limits_changed` flag and would flag the missing `READ_ONCE()`/`WRITE_ONCE()` annotations as data races. This provides static detection without needing to actually trigger the reordering.

- **KTSAN or custom instrumentation**: Thread sanitizer tools or custom tracepoints around the `limits_changed` flag accesses could reveal the race window and confirm the possibility of lost updates.

- **Stress testing on x86 with `mfence` removal**: On x86, while store-store and load-load ordering is guaranteed, store-load reordering is possible. A targeted test that maximizes contention on the `limits_changed` flag while reading policy limits might expose the issue, though it would be very difficult to reproduce reliably.
