# Core: sched_setaffinity Skips user_cpus_ptr Update on Equal Mask

**Commit:** `df14b7f9efcda35e59bb6f50351aac25c50f6e24`
**Affected files:** `kernel/sched/core.c`
**Fixed in:** v6.2
**Buggy since:** v6.2-rc1 (introduced by commit `8f9ea86fdf99` "sched: Always preserve the user requested cpumask")

## Bug Description

The `sched_setaffinity()` system call is supposed to always save the user-requested CPU affinity mask into the task's `user_cpus_ptr` field. This field was introduced by commit `8f9ea86fdf99` to preserve the original user request across cpuset changes: when a cpuset narrows the set of allowed CPUs, the kernel can temporarily restrict a task's affinity, but when the cpuset later expands, the task's affinity should be restored to match what the user originally requested — not just whatever the kernel had narrowed it to.

However, the `__set_cpus_allowed_ptr_locked()` function contains an early-exit optimization: if the new effective CPU mask (`ctx->new_mask`, which is the intersection of the user request and the cpuset-allowed mask) is identical to the task's current `cpus_mask`, the function returns immediately via `goto out` without performing any work. This early exit skips the call to `set_cpus_allowed_common()`, which is the function that performs the `swap(p->user_cpus_ptr, ctx->user_mask)` to save the new user-requested mask.

As a result, when a user calls `sched_setaffinity()` with a mask that, after cpuset intersection, produces the same effective mask as the task already has, the `user_cpus_ptr` is not updated. The task retains its old `user_cpus_ptr` value (which may be `NULL` if no prior `sched_setaffinity()` set it, or may be a stale mask from a previous call). This causes incorrect behavior on subsequent cpuset changes, where the task's affinity would be restored to the wrong mask.

## Root Cause

The root cause lies in the early-exit path in `__set_cpus_allowed_ptr_locked()` in `kernel/sched/core.c`. The relevant code before the fix is:

```c
if (!(ctx->flags & SCA_MIGRATE_ENABLE)) {
    if (cpumask_equal(&p->cpus_mask, ctx->new_mask))
        goto out;
    ...
}
```

When `sched_setaffinity()` is called, it allocates a `user_mask` cpumask, copies the user-provided mask into it, and creates an `affinity_context` with the `SCA_USER` flag set and `ctx->user_mask` pointing to the allocated mask. It then calls `__sched_setaffinity()`, which intersects the user mask with the cpuset-allowed mask to produce `ctx->new_mask`. This `new_mask` is what gets passed to `__set_cpus_allowed_ptr_locked()`.

When the code path reaches `set_cpus_allowed_common()` (called from `__do_set_cpus_allowed()`), it checks the `SCA_USER` flag and performs:

```c
if (ctx->flags & SCA_USER)
    swap(p->user_cpus_ptr, ctx->user_mask);
```

This swap stores the new user-requested mask into `p->user_cpus_ptr` and puts the old `user_cpus_ptr` into `ctx->user_mask` (which gets freed by the caller `sched_setaffinity()` via `kfree(ac.user_mask)`).

However, when `cpumask_equal(&p->cpus_mask, ctx->new_mask)` is true, the early `goto out` bypasses `__do_set_cpus_allowed()` entirely, so the swap never happens. The `user_cpus_ptr` retains whatever value it had before, and the newly allocated `user_mask` in `ctx->user_mask` is freed without being installed.

A concrete scenario that triggers this:

1. A task is in a cpuset allowing CPUs {0,1,2,3}, with `cpus_mask` = {0,1,2,3} and `user_cpus_ptr` = NULL.
2. The user calls `sched_setaffinity(pid, {0,1})`. The effective mask becomes {0,1} ∩ {0,1,2,3} = {0,1}. Since {0,1} ≠ {0,1,2,3}, the normal path executes, `user_cpus_ptr` = {0,1}. Task's `cpus_mask` = {0,1}.
3. The cpuset is narrowed to {0,1}. `set_cpus_allowed_ptr()` is called with {0,1}. Since {0,1} == current `cpus_mask`, nothing changes.
4. The user calls `sched_setaffinity(pid, {0,1,2})`. The effective mask = {0,1,2} ∩ {0,1} = {0,1}. Since {0,1} == current `cpus_mask`, the early exit triggers. **`user_cpus_ptr` is NOT updated to {0,1,2}; it stays as {0,1}.**
5. The cpuset expands back to {0,1,2,3}. The kernel calls `set_cpus_allowed_ptr()` which uses `user_cpus_ptr` to determine the restore mask. It restores affinity to {0,1} instead of {0,1,2}. This is incorrect.

## Consequence

The observable impact is incorrect CPU affinity restoration after cpuset changes. When a cpuset expands, tasks within it should have their affinity restored to whatever the user last explicitly requested via `sched_setaffinity()`. With this bug, the restoration uses a stale `user_cpus_ptr`, meaning the task ends up with a narrower (or otherwise different) affinity than what the user most recently requested.

This is not a crash or kernel panic — it is a silent correctness bug. The task may be confined to fewer CPUs than intended, potentially causing performance degradation. In workloads that rely on precise CPU affinity management (e.g., containerized workloads, DPDK, real-time applications), this could lead to unexpected CPU binding after cpuset reconfiguration events such as CPU hotplug, cgroup migrations, or cpuset partition changes.

In the worst case, if the stale `user_cpus_ptr` is `NULL` (the task never had a prior `sched_setaffinity()` call), the `task_user_cpus()` helper returns `cpu_possible_mask`, which could cause the task to run on any CPU — defeating the purpose of the user's affinity request entirely.

## Fix Summary

The fix adds the `user_cpus_ptr` swap to the early-exit path in `__set_cpus_allowed_ptr_locked()`. Specifically, when `cpumask_equal(&p->cpus_mask, ctx->new_mask)` is true and the `SCA_USER` flag is set, the fix performs the `swap(p->user_cpus_ptr, ctx->user_mask)` before taking the `goto out`:

```c
if (!(ctx->flags & SCA_MIGRATE_ENABLE)) {
    if (cpumask_equal(&p->cpus_mask, ctx->new_mask)) {
        if (ctx->flags & SCA_USER)
            swap(p->user_cpus_ptr, ctx->user_mask);
        goto out;
    }
    ...
}
```

This ensures that even when the effective CPU mask does not change, the user's requested mask is still saved into `p->user_cpus_ptr`. The old `user_cpus_ptr` is placed into `ctx->user_mask`, which will be freed by the caller (`sched_setaffinity()` calls `kfree(ac.user_mask)` after `__sched_setaffinity()` returns). This is exactly the same swap mechanism used by `set_cpus_allowed_common()` — the fix simply replicates it for the early-exit case.

The fix is correct and complete because the early-exit path was the only place where `SCA_USER` calls could return success (ret = 0) without having the swap performed. All other early exits in the function return with `ret = -EINVAL` or `ret = -EBUSY`, which means the `sched_setaffinity()` call failed and no update should occur. The `SCA_MIGRATE_ENABLE` case is also correctly excluded via the outer `if` condition, as migration enable paths do not carry user masks.

## Triggering Conditions

The bug is triggered when:

1. **`sched_setaffinity()` syscall is used** (not kernel-internal `set_cpus_allowed_ptr()`). Only `sched_setaffinity()` sets the `SCA_USER` flag and provides a `user_mask`.

2. **The effective CPU mask (after cpuset intersection) equals the task's current `cpus_mask`.** This can happen when:
   - The user requests the exact same mask the task already has.
   - The user requests a broader mask, but the cpuset intersection narrows it back to the current mask.
   - The user requests a different mask that happens to produce the same effective set after intersection.

3. **A subsequent cpuset change or `relax_compatible_cpus_allowed_ptr()` call uses `user_cpus_ptr` to restore affinity.** Without this step, the stale `user_cpus_ptr` is latent but has no observable effect.

Configuration requirements:
- **CONFIG_SMP=y** (required for `user_cpus_ptr` to be allocated).
- **At least 2 CPUs** in the system.
- **Cpuset cgroups** configured to restrict and then expand CPU masks.

The bug is deterministic — it always occurs when the conditions are met. There is no race condition involved; it is a simple missed code path in the early-exit optimization.

The kernel version must be between v6.2-rc1 (where commit `8f9ea86fdf99` introduced `user_cpus_ptr` management in `sched_setaffinity()`) and v6.2 (where the fix was merged).

## Reproduce Strategy (kSTEP)

Reproducing this bug requires calling `sched_setaffinity()` rather than `set_cpus_allowed_ptr()`, because only `sched_setaffinity()` sets the `SCA_USER` flag that triggers the `user_cpus_ptr` update logic. kSTEP's existing `kstep_task_pin()` uses `set_cpus_allowed_ptr()`, which does not go through the `SCA_USER` path and thus cannot trigger this bug.

**kSTEP Extension Required:** A minor extension is needed: import `sched_setaffinity` via `KSYM_IMPORT` (since it is not `EXPORT_SYMBOL`'d) or define a wrapper that calls the internal `__sched_setaffinity()` with the `SCA_USER` flag set. Alternatively, since `sched_setaffinity()` is a public kernel function (just not exported to modules), it can be imported using kSTEP's `KSYM_IMPORT_TYPED` mechanism:

```c
typedef long (*sched_setaffinity_fn_t)(pid_t, const struct cpumask *);
KSYM_IMPORT_TYPED(sched_setaffinity_fn_t, sched_setaffinity);
```

**Driver Design:**

1. **Topology:** Configure QEMU with at least 4 CPUs. Use default flat topology (no special SMT/NUMA setup needed).

2. **Task Setup:**
   - Create one CFS task: `struct task_struct *p = kstep_task_create();`
   - Pin it initially to CPUs {1,2,3}: `kstep_task_pin(p, 1, 3);`
   - Let it run: `kstep_task_wakeup(p); kstep_tick_repeat(5);`

3. **Cpuset Setup:**
   - Create a cpuset cgroup: `kstep_cgroup_create("testgrp");`
   - Set cpuset to CPUs {1,2}: `kstep_cgroup_set_cpuset("testgrp", "1-2");`
   - Move the task into the cgroup: `kstep_cgroup_add_task("testgrp", p->pid);`
   - Let the cpuset take effect: `kstep_tick_repeat(5);`
   - At this point, the task's `cpus_mask` should be {1,2} (intersection of {1,2,3} user request and {1,2} cpuset). `user_cpus_ptr` should be {1,2,3}.

4. **Trigger the Bug:**
   - Call `KSYM_sched_setaffinity(p->pid, cpumask_of(1) | cpumask_of(2) | cpumask_of(3))` — i.e., set affinity to {1,2,3}. After cpuset intersection, the effective mask is {1,2} ∩ {1,2,3} = {1,2}. Wait: actually the intersection in `__sched_setaffinity` computes `cpumask_and(new_mask, ctx->new_mask, cpus_allowed)` where `cpus_allowed` comes from `cpuset_cpus_allowed()`. If the task is in "testgrp" with cpuset {1,2}, then `cpus_allowed` = {1,2} and `new_mask` = {1,2,3} ∩ {1,2} = {1,2}. Since `cpus_mask` is already {1,2}, the early exit triggers.
   - On the **buggy** kernel: `user_cpus_ptr` is NOT updated to {1,2,3}; it stays as {1,2,3} from step 3 (or potentially a different stale value).

   Actually, in this scenario `user_cpus_ptr` stays the same (both old and new are {1,2,3}). Let me construct a better scenario:

   **Better scenario for observable difference:**
   - Create task, pin to CPU {1}: `kstep_task_pin(p, 1, 1);`
   - Call imported `sched_setaffinity(p->pid, {1})` — sets `user_cpus_ptr` = {1}, `cpus_mask` = {1}.
   - Create cpuset "testgrp" with cpuset {1,2,3}, add task.
   - Call imported `sched_setaffinity(p->pid, {1,2,3})`. Effective mask = {1,2,3} ∩ {1,2,3} = {1,2,3}. But wait, `cpus_mask` was {1}, not {1,2,3}, so this won't trigger the early exit.

   **Simplest trigger scenario:**
   - Task starts with `cpus_mask` = {1,2} (via `kstep_task_pin(p, 1, 2)`).
   - Call `sched_setaffinity(p->pid, {1,2})`. The cpuset allows {1,2} (or all CPUs). Effective mask = {1,2}. Since `cpus_mask` == {1,2}, the early exit triggers. `user_cpus_ptr` is NOT set to {1,2}.
   - Now read `p->user_cpus_ptr`. On buggy kernel: it is NULL (task never had a prior `sched_setaffinity` that went through the full path). On fixed kernel: it is {1,2}.

5. **Detection:**
   - After the `sched_setaffinity()` call, read `p->user_cpus_ptr` directly (kSTEP has full access to internal scheduler state).
   - On the **buggy** kernel: `p->user_cpus_ptr` is NULL (or stale).
   - On the **fixed** kernel: `p->user_cpus_ptr` equals {1,2}.
   - Use `kstep_pass()` / `kstep_fail()` to report the result:
     ```c
     if (p->user_cpus_ptr && cpumask_equal(p->user_cpus_ptr, &expected_mask))
         kstep_pass("user_cpus_ptr correctly updated");
     else
         kstep_fail("user_cpus_ptr not updated: %s",
                    p->user_cpus_ptr ? "wrong value" : "NULL");
     ```

6. **Full Scenario for Observable Scheduling Impact:**
   To demonstrate externally observable impact (not just internal state), extend the test:
   - After the buggy `sched_setaffinity()` call, narrow the cpuset to {1} then expand it to {1,2,3}.
   - When the cpuset expands, `set_cpus_allowed_ptr()` uses `user_cpus_ptr` via `__set_cpus_allowed_ptr()` which intersects the new cpuset mask with `user_cpus_ptr`.
   - On **buggy** kernel: `user_cpus_ptr` is NULL → `task_user_cpus()` returns `cpu_possible_mask` → task gets full cpuset {1,2,3}.
   - On **fixed** kernel: `user_cpus_ptr` is {1,2} → task gets {1,2} ∩ {1,2,3} = {1,2}.
   - Read `p->cpus_mask` after expansion and verify it matches expected value.

7. **Callbacks:** No special callbacks needed. The test is fully sequential:
   - Create task, set initial affinity
   - Call `sched_setaffinity()` with same effective mask
   - Check `user_cpus_ptr`
   - Optionally: modify cpuset, check resulting `cpus_mask`

8. **Expected Output:**
   - **Buggy kernel:** `kstep_fail()` — `user_cpus_ptr` is NULL after `sched_setaffinity()` with an equal mask.
   - **Fixed kernel:** `kstep_pass()` — `user_cpus_ptr` correctly reflects the user request.

9. **Guard Macro:** Since the bug exists from v6.2-rc1 to just before v6.2 final, and the introducing commit `8f9ea86fdf99` first appeared in v6.2-rc1:
   ```c
   #if LINUX_VERSION_CODE >= KERNEL_VERSION(6,2,0)
   ```
   (The feature and the fix both landed in v6.2, so the driver should target v6.2-rc kernels. The exact version guard may need adjustment based on the checked-out kernel.)
