# Cpufreq: Premature kfree of sugov_tunables bypassing kobject release

**Commit:** `e5c6b312ce3cc97e90ea159446e6bfa06645364d`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v5.15-rc1
**Buggy since:** v4.7 (introduced by `9bdcb44e391d` "cpufreq: schedutil: New governor based on scheduler utilization data")

## Bug Description

The schedutil cpufreq governor manages a `struct sugov_tunables` that is embedded within a `struct gov_attr_set`, which in turn contains a `struct kobject`. The kobject is used to expose governor tunables (such as `rate_limit_us`) via sysfs. The kobject lifecycle requires that the underlying memory not be freed until the kobject's reference count drops to zero and its `release()` callback is invoked.

The original `sugov_tunables_free()` function did two things: it cleared the `global_tunables` pointer (if not using per-policy governors) and then called `kfree(tunables)` directly. This direct `kfree()` violated the kobject lifecycle contract. The kobject embedded in the tunables structure might still be referenced — for instance, it could have an active delayed work timer associated with the `gov_attr_set`. When the kobject reference count reaches zero via `gov_attr_set_put()`, the `kobject_put()` that follows triggers the kobject's cleanup path, but the memory has already been freed by the time that cleanup completes.

The bug was present since the initial creation of the schedutil governor in v4.7 (commit `9bdcb44e391d`). The `kobj_type` for `sugov_tunables_ktype` was never given a `.release` callback, meaning there was no proper mechanism for freeing the tunables through the kobject subsystem. Instead, the code relied on manually calling `kfree()` at the point where it believed all references were gone — but this assumption was incorrect because the kobject cleanup is asynchronous.

## Root Cause

The root cause is that `sugov_tunables_free()` called `kfree(tunables)` directly instead of deferring the free to the kobject's `release()` callback. The function was called from two locations:

1. **`sugov_exit()`**: When the governor is exited (e.g., switching to a different governor), `gov_attr_set_put()` is called to decrement the reference count and remove the policy from the attribute set. When the reference count drops to zero, `gov_attr_set_put()` calls `kobject_put()` on the embedded kobject. At this point, the kobject subsystem begins its cleanup, which includes destroying the sysfs entries and eventually calling the type's `release()` callback. However, immediately after `gov_attr_set_put()` returns, `sugov_tunables_free()` calls `kfree()` on the same memory, freeing it while the kobject cleanup may still be in progress.

2. **`sugov_init()` error path**: When `kobject_init_and_add()` fails, the error handler calls `kobject_put()` to undo the init, and then also calls `sugov_tunables_free()` which does `kfree()`. This double-cleanup can similarly corrupt memory.

The specific code in the original `sugov_exit()`:

```c
count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
policy->governor_data = NULL;
if (!count)
    sugov_tunables_free(tunables);  // calls kfree(tunables) directly!
```

When `count` reaches zero, `gov_attr_set_put()` has already called `kobject_put()`, which schedules the kobject's destruction. But `sugov_tunables_free()` then immediately frees the memory containing the kobject. If the kobject still has an active timer (e.g., a `delayed_work` associated with the attribute set), the `debug_check_no_obj_freed()` function detects that an active `timer_list` object is being freed and triggers a warning.

The `kobj_type sugov_tunables_ktype` lacked a `.release` callback entirely:

```c
static struct kobj_type sugov_tunables_ktype = {
    .default_groups = sugov_groups,
    .sysfs_ops = &governor_sysfs_ops,
    // no .release — memory was freed manually!
};
```

Without a `.release` callback, `kobject_put()` does not know how to free the associated memory, so the manual `kfree()` was the only mechanism — but it happened too early.

## Consequence

The observable consequence is a kernel warning from the `CONFIG_DEBUG_OBJECTS` infrastructure. When the schedutil governor is exited (e.g., by writing a different governor name to `/sys/devices/system/cpu/cpufreq/policyN/scaling_governor`), the premature `kfree()` triggers:

```
ODEBUG: free active (active state 0) object type: timer_list hint: delayed_work_timer_fn+0x0/0x30
WARNING: CPU: 3 PID: 720 at lib/debugobjects.c:505 debug_print_object+0xb8/0x100
```

The call trace shows the path: `sugov_exit()` → `kfree()` → `slab_free_freelist_hook()` → `debug_check_no_obj_freed()` → warning. This confirms the memory containing an active `timer_list` (from a `delayed_work` structure) is being freed while the timer is still registered.

Without `CONFIG_DEBUG_OBJECTS`, the bug silently creates a use-after-free condition. The timer callback (`delayed_work_timer_fn`) could fire after the memory has been freed and potentially reallocated for another purpose, leading to memory corruption, kernel crashes, or unpredictable behavior. On production systems without debug options, this could manifest as rare, hard-to-diagnose kernel panics or data corruption, especially under workloads that frequently switch cpufreq governors.

The severity is moderate: the bug is a use-after-free that could lead to kernel memory corruption, but it requires governor switching to trigger, which is not a common operation in most workloads. The race window between the premature free and the timer cleanup is small but non-zero.

## Fix Summary

The fix splits the original `sugov_tunables_free()` function into two separate functions with distinct responsibilities:

1. **`sugov_clear_global_tunables()`**: A new function that only clears the `global_tunables` pointer (setting it to `NULL` when not using per-policy governors). This replaces the `sugov_tunables_free()` calls in `sugov_exit()` and the `sugov_init()` error path. It does NOT free any memory.

2. **`sugov_tunables_free(struct kobject *kobj)`**: A new function with a different signature, designed to serve as the kobject `.release` callback. It extracts the `sugov_tunables` from the kobject via `container_of()` and calls `kfree()` on it. This is registered as the `.release` callback in `sugov_tunables_ktype`.

The updated `sugov_tunables_ktype` now properly delegates memory freeing to the kobject subsystem:

```c
static void sugov_tunables_free(struct kobject *kobj)
{
    struct gov_attr_set *attr_set = container_of(kobj, struct gov_attr_set, kobj);
    kfree(to_sugov_tunables(attr_set));
}

static struct kobj_type sugov_tunables_ktype = {
    .default_groups = sugov_groups,
    .sysfs_ops = &governor_sysfs_ops,
    .release = &sugov_tunables_free,  // now properly set
};
```

This ensures the `kfree()` is called only after the kobject's reference count has reached zero AND all kobject cleanup (including timer destruction and sysfs teardown) has completed. The callers (`sugov_exit()` and the `sugov_init()` error path) now only call `sugov_clear_global_tunables()`, which safely clears the global pointer without touching the memory that may still be in use by the kobject subsystem.

## Triggering Conditions

The bug requires the following conditions to trigger:

- **Active cpufreq driver**: A cpufreq driver must be registered in the kernel (e.g., `acpi-cpufreq`, `intel_pstate` in passive mode, `cppc_cpufreq`, or a platform-specific driver). Without a cpufreq driver, the schedutil governor cannot be activated.
- **Schedutil governor active**: The `schedutil` governor must be the current governor for at least one cpufreq policy. This is typically set via `echo schedutil > /sys/devices/system/cpu/cpufreq/policyN/scaling_governor`.
- **Governor switch or driver removal**: The `sugov_exit()` function must be called. This happens when: (a) a different governor is selected via sysfs (`store_scaling_governor`), (b) the cpufreq driver is removed (e.g., module unload), or (c) the cpufreq policy is cleaned up during CPU hotplug offline.
- **Last reference to tunables**: The bug in `sugov_exit()` only triggers the premature free when `gov_attr_set_put()` returns 0, meaning this is the last policy using the shared tunables. On systems with `!have_governor_per_policy()`, this is the last CPU policy detaching from the shared global tunables.
- **CONFIG_DEBUG_OBJECTS** (for observation): While the use-after-free occurs regardless, the ODEBUG warning that makes the bug visible requires `CONFIG_DEBUG_OBJECTS=y` and `CONFIG_DEBUG_OBJECTS_TIMERS=y` in the kernel configuration.

The bug is reliably triggered by simply switching away from the schedutil governor on the last policy using it. There is no race condition or timing dependency — the premature free happens deterministically every time `sugov_exit()` runs for the last policy.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **QEMU has no cpufreq hardware or driver**: kSTEP runs kernels inside QEMU, which does not emulate CPU frequency scaling hardware. Without a registered cpufreq driver, the schedutil governor cannot be initialized or activated. The entire code path through `sugov_init()`, `sugov_exit()`, and the tunables lifecycle is unreachable without a cpufreq driver providing the `struct cpufreq_policy` objects that drive governor management.

2. **Governor lifecycle requires sysfs interaction**: The bug is triggered by writing to `/sys/devices/system/cpu/cpufreq/policyN/scaling_governor`, which calls `store_scaling_governor()` → `cpufreq_set_policy()` → `cpufreq_exit_governor()` → `sugov_exit()`. kSTEP has `kstep_sysctl_write()` for writing to `/proc/sys/` entries, but this is fundamentally different from sysfs. There is no kSTEP API for interacting with cpufreq sysfs attributes, and adding one would still require a cpufreq driver to be present.

3. **The bug is in cpufreq governor infrastructure, not scheduling behavior**: The observable consequence is a use-after-free of a kobject-protected structure, detected by `CONFIG_DEBUG_OBJECTS`. This is a memory safety bug in the cpufreq governor lifecycle, not a scheduling decision or task behavior anomaly. kSTEP is designed to test scheduling behavior (task ordering, load balancing, bandwidth management), not cpufreq governor object lifecycle management.

4. **What would need to change in kSTEP**: To reproduce this bug, kSTEP would need:
   - A cpufreq driver backend for QEMU (either a fake/dummy cpufreq driver registered at boot, or QEMU hardware emulation of frequency scaling).
   - A new API like `kstep_cpufreq_set_governor(policy, "schedutil")` and `kstep_cpufreq_set_governor(policy, "performance")` that internally calls `cpufreq_set_policy()`.
   - Integration with `CONFIG_DEBUG_OBJECTS` to programmatically detect the ODEBUG warning, or an alternative detection mechanism (e.g., hooking `debug_print_object()` or checking for the use-after-free via KASAN).
   These are **fundamental** changes — adding a cpufreq driver to a QEMU environment that lacks frequency scaling hardware is not a minor extension.

5. **Alternative reproduction methods**: The bug can be reproduced on real hardware (or in a VM with a dummy cpufreq driver) by:
   - Building a kernel with `CONFIG_CPU_FREQ=y`, `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y`, `CONFIG_DEBUG_OBJECTS=y`, and `CONFIG_DEBUG_OBJECTS_TIMERS=y`.
   - Booting with an active cpufreq driver.
   - Setting schedutil as the governor: `echo schedutil > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor`
   - Switching to a different governor: `echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor`
   - Checking `dmesg` for the ODEBUG warning about freeing an active timer_list.
   Alternatively, a custom `cpufreq_dummy` kernel module could be written to register a minimal cpufreq driver, enabling governor lifecycle testing without real hardware. However, integrating such a module into kSTEP's QEMU environment would be a non-trivial architectural addition.
