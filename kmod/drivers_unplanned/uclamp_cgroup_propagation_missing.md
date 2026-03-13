# Uclamp: Missing Effective Uclamp Propagation on New Cgroup Creation

**Commit:** `7226017ad37a888915628e59a84a2d1e57b40707`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.6-rc1
**Buggy since:** v5.4-rc1 (introduced by commit `0b60ba2dd342` "sched/uclamp: Propagate parent clamps")

## Bug Description

When a new CPU cgroup is created with `CONFIG_UCLAMP_TASK_GROUP=y`, the effective uclamp values for the new child group are not properly computed. The `cpu_cgroup_css_online()` callback, which is called when a cgroup comes online, fails to invoke `cpu_util_update_eff()` to propagate the hierarchical effective clamp values down from the parent group.

As a result, the newly created child cgroup retains the `root_task_group`'s raw uclamp values (which are both 1024 for `uclamp_min` and `uclamp_max`) as its effective clamp values, rather than computing the correct effective values by considering the child's own requested clamps and the parent's effective clamps. This means every task placed into the new cgroup inherits an effective `uclamp_min` of 1024 (SCHED_CAPACITY_SCALE), which tells the scheduler and the cpufreq governor (schedutil) that all tasks require maximum CPU performance.

The bug was observed on Ubuntu and Debian systems where the init system (systemd) automatically creates CPU controller cgroup hierarchies and places all system tasks into them at boot time. Since every newly created cgroup gets an effective `uclamp_min` of 1024, the runqueue's aggregated uclamp_min value stays at maximum most of the time, causing the schedutil governor to select the highest CPU frequency permanently — effectively behaving like the performance governor.

The problem was reported by Doug Smythies, who observed that the schedutil governor behaved identically to the performance governor on systems running kernel 5.4+ with `CONFIG_UCLAMP_TASK_GROUP=y`. The issue was architecture-independent and reproduced on both x86 (Intel i7-2600K) and ARM64 (Juno) platforms.

## Root Cause

The root cause lies in the interaction between three functions during cgroup creation: `alloc_uclamp_sched_group()`, `cpu_cgroup_css_online()`, and the missing call to `cpu_util_update_eff()`.

In `init_uclamp()`, the `root_task_group` is initialized with both `uclamp_req` and `uclamp` (effective) values set to `SCHED_CAPACITY_SCALE` (1024) for both `UCLAMP_MIN` and `UCLAMP_MAX`. This is correct for the root group — it represents the system-wide maximum capability.

When a new child cgroup is created, `cpu_cgroup_css_alloc()` calls `sched_create_group()`, which calls `alloc_uclamp_sched_group(tg, parent)`. This function initializes the new task group as follows:

```c
for_each_clamp_id(clamp_id) {
    uclamp_se_set(&tg->uclamp_req[clamp_id],
                  uclamp_none(clamp_id), false);
    tg->uclamp[clamp_id] = parent->uclamp[clamp_id];
}
```

This sets `tg->uclamp_req[UCLAMP_MIN]` to 0 (via `uclamp_none(UCLAMP_MIN)`) and `tg->uclamp_req[UCLAMP_MAX]` to 1024 (via `uclamp_none(UCLAMP_MAX)`). However, it blindly copies the parent's *effective* uclamp values into the child's effective values: `tg->uclamp[UCLAMP_MIN] = parent->uclamp[UCLAMP_MIN]`, which is 1024 for a child of root_task_group.

The `cpu_util_update_eff()` function is the mechanism that correctly computes effective values by taking `min(child_requested, parent_effective)`. For `UCLAMP_MIN`, this would yield `min(0, 1024) = 0`. But in the buggy code, `cpu_cgroup_css_online()` never calls `cpu_util_update_eff()`:

```c
static int cpu_cgroup_css_online(struct cgroup_subsys_state *css)
{
    struct task_group *tg = css_tg(css);
    struct task_group *parent = css_tg(css->parent);

    if (parent)
        sched_online_group(tg, parent);
    return 0;  /* Missing cpu_util_update_eff(css) call! */
}
```

Therefore the child group's effective `uclamp[UCLAMP_MIN]` remains at 1024, which is the value copied from the parent during allocation. When tasks are placed into this cgroup, `uclamp_tg_restrict()` reads `task_group(p)->uclamp[clamp_id]` to determine the effective clamp, and since this is 1024 for `UCLAMP_MIN`, all tasks in the group are boosted to maximum utilization.

The bug is specifically in the ordering of initialization: `alloc_uclamp_sched_group()` copies raw parent values as a placeholder, expecting that `cpu_util_update_eff()` will be called later to compute the correct effective values. But the call was never added to the online path.

## Consequence

The primary consequence is that the schedutil cpufreq governor selects the maximum CPU frequency at all times when any cgroup hierarchy with a CPU controller is active. The runqueue's aggregated `uclamp_min` stays at 1024 (SCHED_CAPACITY_SCALE) because all tasks in newly created cgroups have an effective minimum utilization clamp of 1024.

This results in severe energy waste on mobile and embedded systems where power efficiency is critical, as the CPU never scales down its frequency. On desktop and server systems, it manifests as the schedutil governor behaving identically to the performance governor — the system runs at maximum frequency regardless of actual workload. Doug Smythies demonstrated this with benchmarks showing that with `CONFIG_UCLAMP_TASK_GROUP=y`, the schedutil governor achieved the same throughput as the performance governor (ratio ~1.0-1.1), whereas without the option, schedutil correctly scaled down frequency (ratio ~2.4, similar to powersave).

The bug is particularly impactful on distributions like Ubuntu and Debian that automatically create cpu controller cgroup hierarchies at boot and place all system tasks into them. Even on minimal systems (like Buildroot), manually creating a cpu cgroup and adding tasks to it is sufficient to trigger the frequency lock at maximum. The fix developer (Qais Yousef) confirmed he reproduced this on ARM64 Juno boards with both Debian and Buildroot rootfs.

## Fix Summary

The fix adds a call to `cpu_util_update_eff(css)` in the `cpu_cgroup_css_online()` function, guarded by `#ifdef CONFIG_UCLAMP_TASK_GROUP`. This ensures that when a new cgroup becomes online, its effective uclamp values are immediately recomputed by walking the hierarchy and taking the most restrictive values at each level.

```c
static int cpu_cgroup_css_online(struct cgroup_subsys_state *css)
{
    struct task_group *tg = css_tg(css);
    struct task_group *parent = css_tg(css->parent);

    if (parent)
        sched_online_group(tg, parent);

+#ifdef CONFIG_UCLAMP_TASK_GROUP
+   /* Propagate the effective uclamp value for the new group */
+   cpu_util_update_eff(css);
+#endif

    return 0;
}
```

The `cpu_util_update_eff()` function iterates over the CSS and all its descendants (using `css_for_each_descendant_pre`), computing for each node: `eff[clamp_id] = min(tg->uclamp_req[clamp_id].value, parent->uclamp[clamp_id].value)`. For a newly created child of root_task_group, this correctly computes `eff[UCLAMP_MIN] = min(0, 1024) = 0` and `eff[UCLAMP_MAX] = min(1024, 1024) = 1024`. It then updates `tg->uclamp[clamp_id]` with the correct effective values. This fix is correct and complete because it leverages the existing propagation infrastructure that is already used when uclamp values are written via the cgroup interface or sysctl.

## Triggering Conditions

The bug requires the following conditions:

- **Kernel configuration:** `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y` must both be enabled. `CONFIG_CGROUP_SCHED=y` is also required (implied by UCLAMP_TASK_GROUP).
- **Kernel version:** Any kernel from v5.3-rc1 (commit 0b60ba2dd342) through v5.5-rc3 (just before the fix).
- **Cgroup creation:** At least one CPU controller cgroup must be created as a child of the root cgroup. On distributions like Ubuntu and Debian, systemd does this automatically at boot by creating the cpu controller hierarchy under `/sys/fs/cgroup/` and placing all tasks in child cgroups.
- **Tasks in the cgroup:** At least one task must be placed into the newly created cgroup. The more tasks that run in the buggy cgroup, the more consistently the runqueue's uclamp_min stays at 1024.
- **Schedutil governor:** The frequency impact is only visible when using the schedutil cpufreq governor, which uses uclamp values to influence frequency selection. Other governors (ondemand, conservative) are unaffected as they don't consult uclamp.

The bug is deterministic — it is triggered every single time a new CPU cgroup is created without any timing or race condition requirements. It does not depend on the number of CPUs or topology configuration. It affects both cgroup v1 and v2 hierarchies with CPU controllers.

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced with kSTEP for the following reason:

1. **KERNEL VERSION TOO OLD:** The fix was merged into v5.6-rc1, and the bug existed only in kernels v5.3-rc1 through v5.5-rc3. kSTEP supports Linux v5.15 and newer only. Since the fix has been present in the kernel since v5.6, any kernel that kSTEP can target already contains this fix. There is no way to observe the buggy behavior on v5.15+.

2. **What would be needed:** Even if the kernel version were supported, reproducing this bug in kSTEP would require: (a) creating a cgroup with a CPU controller via `kstep_cgroup_create()`, (b) adding tasks to it, (c) reading the effective uclamp values from the task group structure to verify they are incorrectly set to 1024 for uclamp_min. kSTEP already has `kstep_cgroup_create()` and `kstep_cgroup_add_task()`, so the cgroup manipulation APIs are available. Reading internal `task_group` uclamp fields could be done via `KSYM_IMPORT` and `cpu_rq()` internals. However, the actual frequency selection impact would be hard to observe since kSTEP runs in QEMU without real cpufreq hardware.

3. **Alternative reproduction methods:** The bug was originally reproduced on real hardware by comparing the schedutil governor's frequency behavior between kernels with `CONFIG_UCLAMP_TASK_GROUP=y` and `CONFIG_UCLAMP_TASK_GROUP` unset. The simplest reproducer on affected kernels is:
   - Boot with `CONFIG_UCLAMP_TASK_GROUP=y`
   - Set the schedutil governor: `echo schedutil > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor`
   - Create a cgroup with cpu controller and add tasks to it
   - Observe that the CPU stays at maximum frequency regardless of load
   - Verify by reading `cpu.uclamp.min` which shows 0 (the requested value) but the effective value (internal `tg->uclamp[UCLAMP_MIN].value`) is actually 1024

4. **Internal state verification:** On an affected kernel, one could also verify the bug by reading the task group's effective uclamp values through debugfs or ftrace. The key indicator is that `tg->uclamp[UCLAMP_MIN].value == 1024` for any newly created child cgroup, when it should be 0 (matching the requested value `tg->uclamp_req[UCLAMP_MIN].value`).

5. **Summary:** This is a straightforward initialization bug that is deterministically triggered by cgroup creation, but it existed only in kernels older than what kSTEP supports. The fix was a single function call addition and was backported early in the v5.5 release cycle.
