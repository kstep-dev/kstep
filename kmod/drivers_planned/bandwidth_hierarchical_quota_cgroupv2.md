# Bandwidth: hierarchical_quota Always RUNTIME_INF on cgroupv2

**Commit:** `c98c18270be115678f4295b10a5af5dcc9c4efa0`
**Affected files:** `kernel/sched/core.c`, `kernel/sched/fair.c`, `kernel/sched/sched.h`
**Fixed in:** v6.6-rc1
**Buggy since:** v4.16-rc7 (introduced by commit `c53593e5cb69` "sched, cgroup: Don't reject lower cpu.max on ancestors")

## Bug Description

The `cfs_bandwidth` structure contains a field `hierarchical_quota` (type `s64`) that is meant to reflect the effective bandwidth quota constraining a task group, taking into account limits imposed by any ancestor in the cgroup hierarchy. This field is used by other parts of the scheduler (notably the nohz tick-stop logic) to quickly determine whether a CFS task is subject to any bandwidth limitation without walking the full cgroup tree at runtime.

In cgroupv2, after commit `c53593e5cb69` changed the quota validation logic to allow ancestors to freely set lower `cpu.max` without being rejected by children's configurations, the `tg_cfs_schedulable_down()` function was updated to use `quota = min(quota, parent_quota)` for the cgroupv2 path. However, `RUNTIME_INF` is defined as `((u64)~0ULL)` (i.e., `0xFFFFFFFFFFFFFFFF`), and the `hierarchical_quota` field is `s64`. When `RUNTIME_INF` is stored in an `s64`, it becomes `-1`. The `min()` macro performs signed comparison on `s64` operands, and since `-1` is less than any positive quota value, `min(quota, parent_quota)` always returns `-1` whenever either operand is `RUNTIME_INF`. This means that `hierarchical_quota` ends up as `-1` (i.e., `RUNTIME_INF`) for every task group in cgroupv2, regardless of whether an ancestor actually has a bandwidth limit.

Additionally, when a new task group is created via `alloc_fair_sched_group()`, `init_cfs_bandwidth()` does not initialize `hierarchical_quota` based on the parent's value. The field remains at its default (zero, from `kzalloc`) until `__cfs_schedulable()` is called during the next quota change. This means that between creation of a task group and the first quota modification, `hierarchical_quota` has a stale or meaningless value.

## Root Cause

The root cause has two components:

**1. Signed/unsigned mismatch in `min()` with `RUNTIME_INF`:** In `tg_cfs_schedulable_down()` (in `kernel/sched/core.c`), the cgroupv2 code path was:

```c
if (cgroup_subsys_on_dfl(cpu_cgrp_subsys)) {
    quota = min(quota, parent_quota);
}
```

Here, `quota` and `parent_quota` are both `s64`. `RUNTIME_INF` is `((u64)~0ULL)` = `0xFFFFFFFFFFFFFFFF`. When assigned to `s64 quota`, this value becomes `-1`. The `min()` macro, operating on `s64` values, treats `-1` as smaller than any valid positive quota value (which is a ratio calculated by `normalize_cfs_quota()` using `to_ratio()`). As a result:

- If the child has no limit (`quota == -1`) and parent has a real limit: `min(-1, parent_quota)` = `-1` (should be `parent_quota`).
- If the child has a real limit and parent has no limit (`parent_quota == -1`): `min(quota, -1)` = `-1` (should be `quota`).
- If both have limits: `min(child_quota, parent_quota)` works correctly.
- If neither has limits: `min(-1, -1)` = `-1`, which is correct.

So in any case involving `RUNTIME_INF`, the result is always `-1` = `RUNTIME_INF`, making the field useless.

**2. Missing initialization on task group creation:** The function `init_cfs_bandwidth()` (in `kernel/sched/fair.c`) did not accept any parent information, so the new task group's `cfs_bandwidth.hierarchical_quota` was left as zero (from the `kzalloc` in `alloc_fair_sched_group()`). The field is only updated when `__cfs_schedulable()` runs, which happens when a quota is explicitly changed. Between creation and the first quota change, the field is stale, potentially zero, which is neither `RUNTIME_INF` (no constraint) nor a valid quota ratio.

The cgroupv1 code path was correct because it handled `RUNTIME_INF` explicitly:
```c
if (quota == RUNTIME_INF)
    quota = parent_quota;
else if (parent_quota != RUNTIME_INF && quota > parent_quota)
    return -EINVAL;
```

This code never passes `RUNTIME_INF` through `min()` and thus avoids the signed comparison problem.

## Consequence

The primary observable consequence is that `cfs_b->hierarchical_quota` is `-1` (`RUNTIME_INF`) for all task groups in cgroupv2, regardless of actual bandwidth constraints from ancestors. This means:

1. **Incorrect `cpu.stat` reporting:** The `hierarchical_quota` value is wrong in `/sys/fs/cgroup/.../cpu.stat`, misleading administrators and monitoring tools about actual bandwidth constraints on task groups.

2. **nohz tick-stop interaction (motivation for the fix):** Patch 2/2 of the same series (`Sched/fair: Block nohz tick_stop when cfs bandwidth in use`) uses `hierarchical_quota != RUNTIME_INF` to determine whether a task is under bandwidth control. With the broken `hierarchical_quota`, the nohz code cannot determine that a CFS task has a bandwidth limit at some higher cgroup level, and may incorrectly allow the tick to stop for tasks that need tick-based bandwidth enforcement. When the tick stops on a CPU running a bandwidth-limited task, the task can run far beyond its quota before a remote tick catches up, leading to multi-period stalls where the task is denied runtime for extended periods to compensate for the overrun.

3. **Stale initialization:** Newly created task groups have `hierarchical_quota = 0` (from `kzalloc`) until the first quota change. Any code that reads this field before then gets a meaningless value, which is neither "no limit" nor a valid quota. This is a latent correctness issue that could cause unpredictable behavior in any future code that checks `hierarchical_quota` early in a task group's lifecycle.

## Fix Summary

The fix addresses both root causes:

**1. Correct the `min()` logic in `tg_cfs_schedulable_down()`:** The cgroupv2 path now handles `RUNTIME_INF` explicitly, mirroring the structure of the cgroupv1 path:

```c
if (cgroup_subsys_on_dfl(cpu_cgrp_subsys)) {
    if (quota == RUNTIME_INF)
        quota = parent_quota;
    else if (parent_quota != RUNTIME_INF)
        quota = min(quota, parent_quota);
}
```

This ensures that: (a) If the child has no limit (`RUNTIME_INF`), it inherits the parent's `hierarchical_quota` (which may itself be `RUNTIME_INF` or a real limit from a higher ancestor). (b) If the child has a real limit and the parent also has a real limit, the stricter (smaller) value is used. (c) If the child has a real limit but the parent has `RUNTIME_INF`, the child's limit is kept as-is (the `else if` condition is false, so `quota` remains unchanged). The `min()` call now only operates on two real (positive) quota values, avoiding the signed comparison problem with `-1`.

**2. Propagate parent's `hierarchical_quota` on initialization:** The `init_cfs_bandwidth()` function signature is changed from `init_cfs_bandwidth(struct cfs_bandwidth *cfs_b)` to `init_cfs_bandwidth(struct cfs_bandwidth *cfs_b, struct cfs_bandwidth *parent)`. When `parent` is non-NULL, the new task group inherits `parent->hierarchical_quota`. When `parent` is NULL (the root task group), `hierarchical_quota` is set to `RUNTIME_INF`. The call sites are updated: `sched_init()` passes `NULL` for root, and `alloc_fair_sched_group()` passes `tg_cfs_bandwidth(parent)`. The comment in `tg_cfs_schedulable_down()` is also updated to clarify the semantics.

## Triggering Conditions

The bug is triggered under the following conditions:

- **cgroupv2 must be in use:** The bug only affects the `cgroup_subsys_on_dfl(cpu_cgrp_subsys)` (cgroupv2 / unified hierarchy) code path. cgroupv1 is not affected because it has explicit `RUNTIME_INF` handling.
- **A cgroup hierarchy with at least one bandwidth-limited ancestor:** At minimum, a parent cgroup must have `cpu.max` set to a finite quota (e.g., `"50000 100000"` for 50ms per 100ms), and a child cgroup must exist (with or without its own `cpu.max`).
- **`CONFIG_CFS_BANDWIDTH=y`:** This config must be enabled (it is by default when `CONFIG_CGROUPS` and `CONFIG_FAIR_GROUP_SCHED` are enabled).

The bug is 100% deterministic and does not involve any race conditions or timing sensitivity. Simply creating the hierarchy and setting quotas causes `__cfs_schedulable()` to walk the tree via `tg_cfs_schedulable_down()`, which computes the wrong `hierarchical_quota` for all descendants. The stale initialization issue is similarly deterministic: any newly created task group has `hierarchical_quota = 0` until `__cfs_schedulable()` is first called.

To observe the bug, one can either:
1. Read the `hierarchical_quota` field from `struct cfs_bandwidth` after setting up the hierarchy.
2. Observe the downstream effects (e.g., nohz tick stopping when it shouldn't for bandwidth-limited tasks, though the nohz fix is a separate patch).

The number of CPUs, topology, and workload characteristics are irrelevant for the core bug — it is purely a data-structure computation error in the cgroup bandwidth configuration path.

## Reproduce Strategy (kSTEP)

This bug is straightforward to reproduce in kSTEP because it only requires creating a cgroupv2 hierarchy with bandwidth limits and reading the `hierarchical_quota` field from the `cfs_bandwidth` structure.

### Step-by-step plan:

**1. Cgroup hierarchy setup:**
Create a nested cgroup hierarchy using kSTEP's cgroup API:
```c
kstep_cgroup_create("parent");
kstep_cgroup_create("parent/child");
```

**2. Set a bandwidth limit on the parent:**
Use `kstep_cgroup_write()` to set `cpu.max` on the parent cgroup with a finite quota:
```c
kstep_cgroup_write("parent", "cpu.max", "50000 100000");  // 50ms per 100ms period
```
The child cgroup is left with the default (no limit, i.e., `cpu.max` = `"max 100000"`).

**3. Create a task and place it in the child cgroup:**
This is optional for detecting the core bug (the `hierarchical_quota` field is wrong regardless of whether tasks exist), but useful for completeness:
```c
struct task_struct *t = kstep_task_create();
kstep_cgroup_add_task("parent/child", t->pid);
kstep_task_wakeup(t);
```

**4. Advance time so `__cfs_schedulable()` has been called:**
After setting `cpu.max`, the kernel calls `__cfs_schedulable()` synchronously as part of `tg_set_cfs_bandwidth()`. The write to `cpu.max` via `kstep_cgroup_write()` triggers this path. However, ticking a few times ensures the system is settled:
```c
kstep_tick_repeat(5);
```

**5. Read `hierarchical_quota` from the child's `cfs_bandwidth`:**
Access the child task group's `cfs_bandwidth.hierarchical_quota` using kSTEP's internal access capabilities. We can walk the task group list or use `KSYM_IMPORT` to access `css_tg()`. A simpler approach is to find the child's task group through the task:
```c
#include "internal.h"

struct task_group *child_tg = t->sched_task_group;  // or walk the hierarchy
struct cfs_bandwidth *child_cfs_b = &child_tg->cfs_bandwidth;
s64 hq = child_cfs_b->hierarchical_quota;
```

Similarly, read the parent's `hierarchical_quota`:
```c
struct task_group *parent_tg = child_tg->parent;
s64 parent_hq = parent_tg->cfs_bandwidth.hierarchical_quota;
```

**6. Check pass/fail criteria:**

On the **buggy kernel** (before the fix):
- `parent_tg->cfs_bandwidth.hierarchical_quota` will be the correct ratio (the parent has an explicit limit, so `tg_cfs_schedulable_down()` computes it correctly for the parent itself).
- `child_tg->cfs_bandwidth.hierarchical_quota` will be `RUNTIME_INF` (`(s64)-1` = `(s64)U64_MAX`), because `min(RUNTIME_INF_as_s64, parent_quota)` = `min(-1, parent_quota)` = `-1`.

On the **fixed kernel** (after the fix):
- `parent_tg->cfs_bandwidth.hierarchical_quota` will be the correct ratio.
- `child_tg->cfs_bandwidth.hierarchical_quota` will equal `parent_tg->cfs_bandwidth.hierarchical_quota`, correctly reflecting that the child inherits the parent's constraint.

The check:
```c
if (child_cfs_b->hierarchical_quota == (s64)RUNTIME_INF) {
    kstep_fail("child hierarchical_quota is RUNTIME_INF (%lld), "
               "should inherit parent constraint", hq);
} else if (child_cfs_b->hierarchical_quota == parent_cfs_b->hierarchical_quota) {
    kstep_pass("child hierarchical_quota correctly inherits parent: %lld", hq);
} else {
    kstep_fail("child hierarchical_quota (%lld) != parent (%lld)",
               child_cfs_b->hierarchical_quota,
               parent_cfs_b->hierarchical_quota);
}
```

**7. Additional test: stale initialization:**
To test the initialization fix, create a new child cgroup *without* changing any quota and immediately read its `hierarchical_quota`:
```c
kstep_cgroup_create("parent/child2");
// Do NOT write cpu.max on parent/child2
// Just read hierarchical_quota immediately
struct task_struct *t2 = kstep_task_create();
kstep_cgroup_add_task("parent/child2", t2->pid);
struct cfs_bandwidth *child2_cfs_b = &t2->sched_task_group->cfs_bandwidth;
```
On the buggy kernel, `child2_cfs_b->hierarchical_quota` will be `0` (from `kzalloc`, never updated since no quota was changed). On the fixed kernel, it will be initialized to the parent's `hierarchical_quota` by `init_cfs_bandwidth(cfs_b, parent_cfs_b)`.

**8. Topology and CPU count:**
No special topology is needed. The default 2-CPU QEMU configuration is sufficient. No CPU pinning to CPU 0 is needed (tasks can go to CPU 1).

**9. Callbacks:**
No `on_tick_begin`, `on_sched_softirq_end`, or other callbacks are needed. This is a purely structural bug in the cgroup bandwidth configuration path, observable by reading the `hierarchical_quota` field after setting up the hierarchy.

**10. kSTEP compatibility:**
All required functionality already exists:
- `kstep_cgroup_create()` for creating nested cgroups
- `kstep_cgroup_write()` for setting `cpu.max`
- `kstep_cgroup_add_task()` for placing tasks in cgroups
- `internal.h` provides access to `struct task_group`, `struct cfs_bandwidth`, and `hierarchical_quota`
- `kstep_pass()`/`kstep_fail()` for reporting results

No kSTEP extensions are needed.
