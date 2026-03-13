# Uclamp: Missing Locking in cpu_cgroup_css_online() Around cpu_util_update_eff()

**Commit:** `93b73858701fd01de26a4a874eb95f9b7156fd4b`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.14-rc1
**Buggy since:** v5.6-rc1 (introduced by commit `7226017ad37a` "sched/uclamp: Fix a bug in propagating uclamp value in new cgroups")

## Bug Description

When a new CPU cgroup is brought online with `CONFIG_UCLAMP_TASK_GROUP` enabled, the kernel calls `cpu_cgroup_css_online()` to initialize the task group and propagate effective uclamp values down the cgroup hierarchy. Commit `7226017ad37a` added a call to `cpu_util_update_eff(css)` inside `cpu_cgroup_css_online()` to ensure newly created cgroups inherit the correct effective uclamp min/max values from their parents, rather than defaulting to the root task group's maximum values (1024 for both uclamp_min and uclamp_max).

However, this call to `cpu_util_update_eff()` was added without the required locking. Every other call site of `cpu_util_update_eff()` is protected by both the `uclamp_mutex` (to serialize concurrent reads and writes to the cgroup hierarchy's uclamp values) and `rcu_read_lock()` (to safely traverse the cgroup CSS data structures). The `cpu_cgroup_css_online()` path omitted both of these locks, creating a locking inconsistency and potential for data races.

The `uclamp_mutex` is a global mutex that serializes all uclamp cgroup operations — including writes from `cpu_uclamp_min_write()`/`cpu_uclamp_max_write()` via the cgroup filesystem, and updates from `sysctl_sched_uclamp_handler()` via the sysctl interface. Without holding this mutex, the `cpu_cgroup_css_online()` path could race with concurrent uclamp value modifications, leading to inconsistent effective uclamp propagation. The `rcu_read_lock()` is required because `cpu_util_update_eff()` calls `css_for_each_descendant_pre()` which traverses cgroup CSS pointers that are RCU-protected.

## Root Cause

The function `cpu_util_update_eff()` in `kernel/sched/core.c` walks the cgroup hierarchy starting from a given CSS node, computing the effective uclamp values for each descendant. It does so using `css_for_each_descendant_pre(css, top_css)`, which iterates over the cgroup subsystem state tree. This iteration requires RCU read-side protection because the cgroup CSS tree is an RCU-protected data structure — CSS nodes can be freed asynchronously via RCU callbacks.

Additionally, `cpu_util_update_eff()` reads and writes `css_tg(css)->uclamp_req[clamp_id].value` and `css_tg(css)->uclamp[clamp_id].value` (the effective uclamp values). These fields are also accessed and modified by `cpu_uclamp_write()` (when a user writes to `cpu.uclamp.min` or `cpu.uclamp.max` cgroup files) and by `uclamp_update_root_tg()` (when the root task group's uclamp values are updated via sysctl). All of these paths hold `uclamp_mutex` to serialize access.

In the buggy code, `cpu_cgroup_css_online()` called `cpu_util_update_eff(css)` with neither `uclamp_mutex` nor `rcu_read_lock()`:

```c
static int cpu_cgroup_css_online(struct cgroup_subsys_state *css)
{
    ...
#ifdef CONFIG_UCLAMP_TASK_GROUP
    /* Propagate the effective uclamp value for the new group */
    cpu_util_update_eff(css);  // No locks held!
#endif
    return 0;
}
```

Compare this with `cpu_uclamp_write()` which correctly locks:

```c
mutex_lock(&uclamp_mutex);
rcu_read_lock();
...
cpu_util_update_eff(of_css(of));
rcu_read_unlock();
mutex_unlock(&uclamp_mutex);
```

And `sysctl_sched_uclamp_handler()` which also holds `uclamp_mutex` when calling `uclamp_update_root_tg()`, which in turn takes `rcu_read_lock()` before calling `cpu_util_update_eff()`.

The race condition occurs when `cpu_cgroup_css_online()` runs concurrently with either a cgroup uclamp write or a sysctl update. For instance, if a user creates a new cgroup (triggering `css_online`) while simultaneously writing to `cpu.uclamp.min` of a parent cgroup, the `cpu_util_update_eff()` traversal in `css_online` could see partially updated parent uclamp values, or traverse a CSS tree that is being modified without RCU protection.

## Consequence

The immediate practical consequence is a potential data race on the uclamp effective values during concurrent cgroup creation and uclamp configuration. This could manifest as:

1. **Incorrect effective uclamp propagation**: A newly created cgroup could inherit stale or partially updated effective uclamp values from its parent, causing tasks in that cgroup to run at incorrect frequency/performance points. For example, if the parent's effective `uclamp_max` is being lowered concurrently, the new child cgroup might temporarily see the old higher value, causing its tasks to run at a higher frequency than intended.

2. **Use-after-free or invalid pointer dereference**: Without `rcu_read_lock()`, the `css_for_each_descendant_pre()` iteration could dereference a CSS pointer that has been freed by an RCU callback, leading to a kernel crash (NULL pointer dereference or use-after-free). This is the more severe theoretical consequence, though in practice the timing window is narrow.

3. **Lockdep warnings**: With `CONFIG_LOCKDEP` enabled, the kernel would detect the missing locks and produce warnings about RCU usage violations and mutex assertion failures (after the fix adds `lockdep_assert_held()` and `SCHED_WARN_ON()`).

According to the LKML thread, Quentin Perret from Google discovered the locking oddity by code inspection. The patch author (Qais Yousef) noted in the email thread: "There was no real failure observed because of this. Quentin just observed the oddity and reported it." Nevertheless, the theoretical race window exists and could be triggered on systems that heavily create/destroy cgroups while simultaneously adjusting uclamp parameters.

## Fix Summary

The fix adds the required locking around the `cpu_util_update_eff()` call in `cpu_cgroup_css_online()`:

```c
#ifdef CONFIG_UCLAMP_TASK_GROUP
    /* Propagate the effective uclamp value for the new group */
    mutex_lock(&uclamp_mutex);
    rcu_read_lock();
    cpu_util_update_eff(css);
    rcu_read_unlock();
    mutex_unlock(&uclamp_mutex);
#endif
```

This makes `cpu_cgroup_css_online()` consistent with all other call sites of `cpu_util_update_eff()`. The `uclamp_mutex` ensures that no concurrent uclamp writes (from cgroup filesystem or sysctl) can modify the hierarchy's requested or effective uclamp values while this function is traversing and updating them. The `rcu_read_lock()` ensures safe traversal of the RCU-protected CSS tree.

Additionally, the fix adds two runtime assertions at the top of `cpu_util_update_eff()` itself: `lockdep_assert_held(&uclamp_mutex)` and `SCHED_WARN_ON(!rcu_read_lock_held())`. These assertions document the locking requirements for future maintainers and will trigger warnings if any future caller forgets to acquire the necessary locks, preventing the same class of bug from recurring.

## Triggering Conditions

- **Kernel configuration**: `CONFIG_UCLAMP_TASK_GROUP=y` (enables uclamp support for task groups/cgroups). This is typically enabled on mobile/embedded systems (Android, Chrome OS) that use Energy Aware Scheduling.
- **Cgroup v1 or v2 with CPU controller**: The system must have the CPU cgroup controller enabled and in use.
- **Concurrent operations**: The race requires creating a new cgroup (triggering `cpu_cgroup_css_online()`) concurrently with a uclamp write to an existing cgroup (e.g., `echo 512 > /sys/fs/cgroup/cpu/parent/cpu.uclamp.max`) or a sysctl update (`sysctl kernel.sched_util_clamp_min=XXX`).
- **CPU count**: At least 2 CPUs are needed for the race (one CPU creating the cgroup, another writing uclamp values).
- **Timing**: The race window is narrow — `cpu_util_update_eff()` must be executing its CSS tree traversal at exactly the moment another thread is modifying uclamp values under `uclamp_mutex`. In practice, this is difficult to trigger reliably.
- **Lockdep**: With `CONFIG_LOCKDEP=y`, the missing lock assertions can be detected without the actual race occurring, simply by creating a cgroup while uclamp task groups are enabled.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?** The fix was merged in v5.14-rc1. Since kSTEP supports Linux v5.15 and newer only, the buggy code (without the fix) does not exist in any kernel version that kSTEP can run. A v5.15 kernel already includes this fix, so `cpu_cgroup_css_online()` already holds `uclamp_mutex` and `rcu_read_lock()` in all supported kernels.

2. **WHAT would need to be added to kSTEP?** Even if the kernel version were supported, this bug would be extremely difficult to reproduce in kSTEP because:
   - It requires triggering a race condition between concurrent cgroup creation and uclamp value writes. kSTEP's `kstep_cgroup_create()` and potential uclamp sysctl writes would need to be executed from separate threads with precise timing.
   - The actual consequence (corrupted effective uclamp values or use-after-free) was never observed in practice — it was found by code review.
   - Lockdep-based detection would work but is a build-time configuration check, not a scheduling behavior test.

3. **The primary reason is kernel version too old**: The fix is in v5.14-rc1, which predates kSTEP's minimum supported version of v5.15.

4. **Alternative reproduction methods**: On a v5.13 or earlier kernel with `CONFIG_UCLAMP_TASK_GROUP=y` and `CONFIG_LOCKDEP=y`, simply creating a new CPU cgroup would trigger a lockdep warning about `rcu_read_lock` not being held inside `cpu_util_update_eff()`. Without lockdep, a stress test rapidly creating/destroying cgroups while writing uclamp values could potentially trigger corrupted effective clamp values, but this was never demonstrated in practice.
