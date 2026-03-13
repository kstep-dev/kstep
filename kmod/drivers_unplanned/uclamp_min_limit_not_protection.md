# Uclamp: cpu.uclamp.min Implemented as Limit Instead of Protection

**Commit:** `0c18f2ecfcc274a4bcc1d122f79ebd4001c3b445`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.14-rc1
**Buggy since:** v5.4-rc1 (commit `3eac870a3247` "sched/uclamp: Use TG's clamps to restrict TASK's clamps")

## Bug Description

The cgroup v2 resource distribution model defines `cpu.uclamp.min` as a **protection** — a guarantee that the scheduler will try its best to preserve the minimum performance point of tasks in the group. However, the implementation in `uclamp_tg_restrict()` treated `cpu.uclamp.min` as a **limit** (an upper bound), which is the semantic of `cpu.uclamp.max`. This meant that the task group's `uclamp_min` value acted as a ceiling on per-task `uclamp_min` requests rather than a floor, producing incorrect effective clamp values for tasks in cgroups.

Concretely, consider a task group with `cpu.uclamp.min = 20%` containing two tasks: `p0` with `uclamp[UCLAMP_MIN] = 0` and `p1` with `uclamp[UCLAMP_MIN] = 50%`. With the buggy code, `p0` got an effective uclamp_min of 0 (not boosted to the group's 20% protection floor) and `p1` got an effective uclamp_min of 20% (capped down from its requested 50%). The correct behavior is that `p0` should be boosted to 20% (the group's protection floor) and `p1` should retain its 50% request (which already exceeds the protection floor).

Additionally, the buggy code had a secondary issue: the `!uc_req.user_defined` check caused tasks whose uclamp values were set by the kernel (not explicitly by userspace) — most notably RT tasks that are boosted to max by default — to have their uclamp values silently overridden by the task group value whenever they were attached to a cgroup. This meant RT tasks in a cgroup with `cpu.uclamp.min = 20%` would have their default boost of 1024 (100%) replaced by the TG's 20%, even though no explicit per-task uclamp value had been set via `sched_setattr()`.

## Root Cause

The root cause lies in the `uclamp_tg_restrict()` function in `kernel/sched/core.c`. This function computes the effective uclamp value for a task by combining the task's requested uclamp value (`p->uclamp_req[clamp_id]`) with the task group's uclamp value (`task_group(p)->uclamp[clamp_id]`).

The buggy code used a single, unified comparison for both `UCLAMP_MIN` and `UCLAMP_MAX`:

```c
uc_max = task_group(p)->uclamp[clamp_id];
if (uc_req.value > uc_max.value || !uc_req.user_defined)
    return uc_max;
```

This logic treats the task group's clamp value as an upper bound for **both** clamp types. For `UCLAMP_MAX`, this is correct: if the task requests more than the group allows, cap it. But for `UCLAMP_MIN`, this is backwards. The task group's `uclamp_min` is a protection floor — it should boost tasks whose request is *below* the floor, not cap tasks whose request is *above* it.

The `!uc_req.user_defined` fallback condition compounds the problem. When a task's uclamp value was not explicitly set via `sched_setattr()` (i.e., `user_defined == false`), the code unconditionally replaced the task's request with the task group value. For RT tasks, whose `uclamp_min` is initialized to `sysctl_sched_util_clamp_min_rt_default` (default 1024 = 100%) without `user_defined` being set, this meant that simply moving an RT task into a cgroup with `cpu.uclamp.min = 20%` would downgrade the RT task's boost from 100% to 20%.

The correct semantics, as defined by the cgroup v2 resource distribution model, are:
- `UCLAMP_MIN` (protection): if the task's request is **less than** the group floor, boost it up to the floor; if the task requests more, let it keep its higher value.
- `UCLAMP_MAX` (limit): if the task's request is **greater than** the group ceiling, cap it down to the ceiling; if the task requests less, let it keep its lower value.

## Consequence

The observable impact is incorrect CPU frequency selection and task scheduling decisions for tasks in cgroups with `cpu.uclamp.min` set. Tasks that should receive a minimum performance guarantee from their cgroup instead get capped at the cgroup's value, potentially receiving lower CPU frequency and compute capacity than intended.

Specifically:
1. **Performance degradation for tasks requesting above-floor minimums**: A task with `uclamp_min = 50%` in a cgroup with `cpu.uclamp.min = 20%` would be incorrectly capped to 20%, causing schedutil to select a lower CPU frequency. The task would run slower than its explicitly requested performance level.
2. **No protection for tasks requesting zero minimum**: A task with `uclamp_min = 0` in a cgroup with `cpu.uclamp.min = 20%` would receive no boost, defeating the purpose of the cgroup protection. The task would run at whatever frequency the load average suggests, even when the cgroup intended to guarantee at least 20% performance.
3. **RT task boost silently overridden**: RT tasks boosted to max by default (`sysctl_sched_util_clamp_min_rt_default = 1024`) would lose their boost when placed in any cgroup with a lower `cpu.uclamp.min`. Since the RT tasks' uclamp values have `user_defined = false`, the cgroup's value would always override them.

This bug does not cause crashes or hangs, but it causes incorrect scheduling behavior that violates the cgroup v2 resource distribution model's contract. On systems using uclamp for performance management (e.g., Android, Chromebook, embedded ARM platforms), this could lead to unexpected performance regressions for cgroup-managed workloads.

## Fix Summary

The fix replaces the unified comparison in `uclamp_tg_restrict()` with a `switch` statement that handles `UCLAMP_MIN` and `UCLAMP_MAX` with different semantics:

```c
switch (clamp_id) {
case UCLAMP_MIN: {
    struct uclamp_se uc_min = task_group(p)->uclamp[clamp_id];
    if (uc_req.value < uc_min.value)
        return uc_min;
    break;
}
case UCLAMP_MAX: {
    struct uclamp_se uc_max = task_group(p)->uclamp[clamp_id];
    if (uc_req.value > uc_max.value)
        return uc_max;
    break;
}
default:
    WARN_ON_ONCE(1);
    break;
}
```

For `UCLAMP_MIN`, the comparison is now `uc_req.value < uc_min.value` — if the task's request is below the group's protection floor, use the floor. Otherwise, keep the task's higher request. This implements the correct "protection" (lower-bound guarantee) semantic.

For `UCLAMP_MAX`, the comparison remains `uc_req.value > uc_max.value` — if the task's request exceeds the group's ceiling, cap it. This maintains the correct "limit" (upper-bound restriction) semantic.

The fix also removes the `!uc_req.user_defined` condition entirely. With `UCLAMP_MIN` now correctly acting as a protection, the `user_defined` flag is no longer needed: RT tasks in a cgroup will have their boost preserved (since their default max boost exceeds any reasonable `cpu.uclamp.min` floor), and the cgroup can still control RT task performance via `cpu.uclamp.max`. A `default` case with `WARN_ON_ONCE(1)` is added to catch any hypothetical invalid `clamp_id` values.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

1. **Kernel configuration**: `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y` must both be enabled. These are typically enabled on ARM-based systems (Android, Chromebook) but are disabled by default on most x86 distributions.

2. **Cgroup v2 with cpu controller**: A non-root cgroup must exist with the `cpu` controller enabled. The cgroup must have `cpu.uclamp.min` set to a non-zero value. Tasks in the root task group or in autogroups are not affected (they bypass `uclamp_tg_restrict()` entirely).

3. **Task with per-task uclamp_min higher than the TG value**: A task must have `uclamp[UCLAMP_MIN]` set to a value greater than the task group's `cpu.uclamp.min`. This can be set via `sched_setattr()` with the `SCHED_FLAG_UTIL_CLAMP_MIN` flag. On the buggy kernel, the task's effective uclamp_min will be capped to the TG value instead of being preserved.

4. **Alternatively, a task with no explicit uclamp (user_defined=false)**: Any task that has not had its uclamp explicitly set via `sched_setattr()` will have its uclamp silently replaced by the TG value. This is particularly impactful for RT tasks, which default to `uclamp_min = sysctl_sched_util_clamp_min_rt_default` (1024) without `user_defined` being set.

The bug is deterministic and 100% reproducible. There are no race conditions or timing dependencies — the incorrect behavior occurs every time `uclamp_eff_get()` → `uclamp_tg_restrict()` is called for a task in a non-root cgroup, which happens on every enqueue via `uclamp_cpu_inc()`.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **Kernel version too old**: The fix was merged into v5.14-rc1, and the bug existed from v5.4-rc1 through v5.13.x. kSTEP supports Linux v5.15 and newer only. The buggy kernel versions (v5.4 through v5.13) are all before kSTEP's minimum supported version. On v5.15, this bug is already fixed.

2. **If the version constraint were removed**, the bug would be straightforward to reproduce with kSTEP. The framework already provides all necessary capabilities:
   - `kstep_cgroup_create()` to create a child cgroup
   - `kstep_cgroup_write()` to set `cpu.uclamp.min` on the cgroup
   - `kstep_task_create()` to create CFS tasks
   - `kstep_cgroup_add_task()` to move tasks into the cgroup
   - Access to `p->uclamp[UCLAMP_MIN].value` and `uclamp_eff_value(p, UCLAMP_MIN)` via `internal.h` to read the effective uclamp value

3. **Hypothetical reproduction plan** (if the kernel version were supported):
   - Create a child cgroup and set `cpu.uclamp.min` to 204 (20% of 1024).
   - Create task `p0` with no explicit uclamp setting (default `uclamp_min = 0`).
   - Create task `p1` and set its `uclamp_min` to 512 (50%) via `sched_setattr()` or direct internal manipulation.
   - Move both tasks into the cgroup.
   - Enqueue both tasks (wake them up).
   - Read `uclamp_eff_value(p0, UCLAMP_MIN)`: on the buggy kernel it would be 0, on the fixed kernel it would be 204.
   - Read `uclamp_eff_value(p1, UCLAMP_MIN)`: on the buggy kernel it would be 204, on the fixed kernel it would be 512.

4. **No kSTEP extensions needed**: The existing API is sufficient to reproduce this bug. The only blocker is the kernel version constraint. If kSTEP's minimum version were lowered to v5.4, or if this patch were reverted on a v5.15+ kernel for testing purposes, the bug could be reproduced with a simple ~30-line driver.

5. **Alternative reproduction outside kSTEP**: The bug can be reproduced on any v5.4–v5.13 kernel with `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y` using standard userspace tools:
   ```bash
   # Create a cgroup with cpu.uclamp.min = 20%
   mkdir /sys/fs/cgroup/test
   echo 204 > /sys/fs/cgroup/test/cpu.uclamp.min
   # Create a task with uclamp_min = 50% (using schedtool or custom sched_setattr wrapper)
   # Move it into the cgroup
   echo $PID > /sys/fs/cgroup/test/cgroup.procs
   # Read effective uclamp from /proc/$PID/sched or trace uclamp_cpu_inc
   ```
   On the buggy kernel, the effective uclamp_min will be 204 (capped to TG value). On the fixed kernel, it will be 512 (task's own higher request preserved).
