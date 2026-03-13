# Core: Deferred CPU Pick in migration_cpu_stop() Selects Offline CPU

**Commit:** `475ea6c60279e9f2ddf7e4cf2648cd8ae0608361`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.14-rc1
**Buggy since:** v5.10-rc1 (introduced by commit `6d337eab041d` "sched: Fix migrate_disable() vs set_cpus_allowed_ptr()")

## Bug Description

When `set_cpus_allowed_ptr()` (SCA) is called to change a task's CPU affinity, the function carefully selects a valid destination CPU that satisfies the new affinity mask while also being online and active. However, when the task is currently running and cannot be immediately migrated, the migration must be deferred to the stopper thread via `migration_cpu_stop()`. The bug is that the carefully chosen destination CPU was being discarded in this deferred path, replaced with a sentinel value of `-1` meaning "pick any CPU later."

The deferred CPU selection in `migration_cpu_stop()` used `cpumask_any_distribute(&p->cpus_mask)` to pick a CPU from the task's new affinity mask. This function simply selects any bit set in the mask without checking whether that CPU is online or active. If the affinity mask contains offline CPUs (which is perfectly valid — the mask represents the user's requested affinity, not the set of usable CPUs), the function may select an offline CPU. When this happens, `__migrate_task()` checks `is_cpu_allowed()` which verifies the CPU is in the active mask, and silently refuses to perform the migration.

The consequence is that the task remains on its current CPU, which may be outside its newly requested affinity mask. This violates the fundamental guarantee that `set_cpus_allowed_ptr()` (and by extension `sched_setaffinity()` / `taskset`) will not return until the task is running within the specified mask. The bug is particularly insidious because it fails silently — there is no error return or warning, the migration simply does not happen.

Additionally, when a second `set_cpus_allowed_ptr()` call occurs while a migration is already pending, the existing pending migration's destination CPU was never updated. This means even the carefully chosen dest_cpu from the first SCA call could become stale, and the second call's dest_cpu was completely ignored, creating a window where the pending completion fires while the task is on a disallowed CPU.

## Root Cause

The root cause is in the `affine_move_task()` function in `kernel/sched/core.c`. When setting up a new `migration_pending` structure for a task that needs to be migrated by the stopper thread, the code initialized the `migration_arg` with `dest_cpu = -1`:

```c
my_pending.arg = (struct migration_arg) {
    .task = p,
    .dest_cpu = -1,     /* any */
    .pending = &my_pending,
};
```

This `-1` sentinel caused `migration_cpu_stop()` to enter a special branch that re-selected the destination CPU:

```c
if (dest_cpu < 0) {
    if (cpumask_test_cpu(task_cpu(p), &p->cpus_mask))
        goto out;
    dest_cpu = cpumask_any_distribute(&p->cpus_mask);
}
```

The `cpumask_any_distribute()` function iterates over the bits set in `p->cpus_mask` but does not check `cpu_active_mask`. If CPUs in the affinity mask have been taken offline between when `set_cpus_allowed_ptr()` was called and when the stopper runs, this function can return an offline CPU number. The subsequent call to `__migrate_task()` then checks `is_cpu_allowed(p, dest_cpu)` which calls `cpu_active(dest_cpu)`, finds the CPU is not active, and returns without migrating.

A second dimension of the root cause concerns concurrent affinity changes. When `affine_move_task()` finds that a `migration_pending` already exists (because a previous SCA call installed one), it merely incremented the reference count but did not update the pending's `dest_cpu`:

```c
} else {
    pending = p->migration_pending;
    refcount_inc(&pending->refs);
    /* No update to pending->arg.dest_cpu! */
}
```

This meant that if task affinity was changed twice in quick succession (first SCA installs pending with dest_cpu=-1, second SCA joins the pending without updating dest_cpu), the stopper thread would still use the original `-1` sentinel value, potentially picking an offline CPU that was valid for the first affinity change but not the second.

The interaction between these two problems is what makes the bug reliably triggerable: the carefully computed `dest_cpu` from `set_cpus_allowed_ptr()` (which accounts for online/active state) is discarded, and the re-selection in the stopper thread does not apply the same constraints.

## Consequence

The primary consequence is that a task can become stranded on a CPU that is outside its affinity mask. The `set_cpus_allowed_ptr()` call returns successfully (or rather, the completion fires indicating the migration is "done"), but the task has not actually moved. This violates the userspace-visible contract of `sched_setaffinity()`: after the syscall returns, the task is guaranteed to be running within the specified CPU set.

The specific scenario described in the commit message demonstrates this clearly:

1. A task on CPU 0 has affinity set to CPUs 0-2 via `taskset -pc 0-2 $PID`
2. CPUs 3-4 are taken offline
3. The affinity is then changed to CPUs 3-5 via `taskset -pc 3-5 $PID`
4. `set_cpus_allowed_ptr()` correctly identifies CPU 5 as the destination (since it's the only online CPU in the new mask)
5. But `affine_move_task()` stores `dest_cpu = -1`, discarding CPU 5
6. `migration_cpu_stop()` calls `cpumask_any_distribute({3,4,5})` which can return CPU 3 or 4 (both offline)
7. `__migrate_task()` refuses to migrate to the offline CPU
8. The task stays on CPU 0, which is outside mask {3,4,5}

This can lead to incorrect NUMA placement, violation of cpuset constraints, incorrect load distribution, and on asymmetric systems (like ARM big.LITTLE), tasks running on CPUs with the wrong capabilities. Will Deacon specifically discovered this while working on asymmetric AArch32 support where 32-bit tasks must be constrained to CPUs with 32-bit execution support.

## Fix Summary

The fix has two key changes, both in `kernel/sched/core.c`:

**First**, in `affine_move_task()`, when creating a new `migration_pending`, the code now stores the actual `dest_cpu` that was carefully selected by `set_cpus_allowed_ptr()` instead of the `-1` sentinel:

```c
my_pending.arg = (struct migration_arg) {
    .task = p,
    .dest_cpu = dest_cpu,   /* was: -1 */
    .pending = &my_pending,
};
```

When joining an existing pending (second concurrent SCA call), the fix updates the pending's destination CPU to reflect the new affinity:

```c
} else {
    pending = p->migration_pending;
    refcount_inc(&pending->refs);
    pending->arg.dest_cpu = dest_cpu;  /* NEW: update dest */
}
```

This update is safe because it is serialized by `p->pi_lock`, which both `affine_move_task()` and `migration_cpu_stop()` hold.

**Second**, in `migration_cpu_stop()`, the special `dest_cpu < 0` branch that called `cpumask_any_distribute()` is removed entirely. The function now always uses `arg->dest_cpu` directly. The "is the task already on an allowed CPU?" check is moved to only apply when there is a `pending` (the case where the task may have moved to a valid CPU between SCA and the stopper executing):

```c
if (pending) {
    p->migration_pending = NULL;
    complete = true;
    if (cpumask_test_cpu(task_cpu(p), &p->cpus_mask))
        goto out;
}
if (task_on_rq_queued(p))
    rq = __migrate_task(rq, &rf, p, arg->dest_cpu);
else
    p->wake_cpu = arg->dest_cpu;
```

This fix is correct because `set_cpus_allowed_ptr()` already performs thorough validation of the destination CPU (checking online/active state, cpuset constraints, etc.), so there is no need to re-select in the stopper. Note that a race with CPU hotplug between SCA and the stopper execution is still possible — this is addressed in the companion patch 2/2 of the same series, which adds a `select_fallback_rq()` call in the stopper if the chosen dest_cpu has become disallowed.

## Triggering Conditions

The bug requires the following specific conditions:

- **Kernel version**: v5.10-rc1 through v5.13.x (after commit `6d337eab041d`, before fix `475ea6c60279`)
- **CONFIG_CPUSET=n** (or at least not constraining the affinity mask against `cpu_online_mask`), so that `set_cpus_allowed_ptr()` accepts an affinity mask that contains offline CPUs
- **At least one offline CPU** whose number is included in the new affinity mask. This requires CPU hotplug capability to take CPUs offline before changing affinity.
- **The task must be currently running** (or in `TASK_WAKING` state) when `set_cpus_allowed_ptr()` is called, so that the migration is deferred to the stopper thread via `affine_move_task()`. If the task is not running, it can be migrated immediately without the stopper.
- **The affinity mask must include at least one offline CPU** and the `cpumask_any_distribute()` function must select that offline CPU. Since `cpumask_any_distribute()` is deterministic for a given mask and per-CPU distribution state, this is not a probabilistic race but a deterministic failure when offline CPUs appear before online ones in the mask iteration order.

The triggering scenario is:
1. Start with a task pinned to CPUs 0-2
2. Take CPUs 3 and 4 offline (via `/sys/devices/system/cpu/cpu{3,4}/online`)
3. Change the task's affinity to CPUs 3-5 while the task is running
4. The stopper thread picks CPU 3 or 4 (offline) from `cpumask_any_distribute({3,4,5})`
5. Migration fails silently; task remains on its current CPU (outside mask)

No race condition timing is required — the bug triggers deterministically once offline CPUs are in the mask and the stopper selects one.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **KERNEL VERSION TOO OLD**: The fix was merged into v5.14-rc1 (committed to the `sched/core` branch of the tip tree on 2021-06-01, released in v5.14-rc1). kSTEP supports Linux v5.15 and newer only. The buggy kernel versions (v5.10 through v5.13) are all older than v5.15, making it impossible to check out a kernel that both has the bug and is compatible with kSTEP's infrastructure.

2. **Requires CPU hotplug**: Even if the kernel version were compatible, reproducing this bug requires taking CPUs offline to create the condition where the affinity mask contains offline CPUs. kSTEP does not have a `kstep_cpu_offline()` or `kstep_cpu_hotplug()` API. While it might be possible to call `cpu_down()` via `KSYM_IMPORT`, CPU hotplug has complex interactions with the stopper thread infrastructure that may not work correctly from within a kernel module running in the QEMU environment.

3. **Requires set_cpus_allowed_ptr() on a running task via stopper path**: The bug only manifests when the migration goes through `affine_move_task()` → stopper → `migration_cpu_stop()`. This requires the target task to be currently running on a CPU when its affinity is changed. While kSTEP can create running tasks, triggering the specific affine_move_task code path requires careful orchestration of task state that depends on the stopper thread mechanism.

4. **Alternative reproduction outside kSTEP**: The bug can be reproduced on a real or virtual machine running a v5.10-v5.13 kernel (compiled with CONFIG_CPUSET=n) using the following script:
   ```bash
   # Start a CPU-bound task
   taskset -c 0 yes > /dev/null &
   PID=$!
   # Set initial affinity
   taskset -pc 0-2 $PID
   # Offline some CPUs
   echo 0 > /sys/devices/system/cpu/cpu3/online
   echo 0 > /sys/devices/system/cpu/cpu4/online
   # Change affinity to include offline CPUs
   taskset -pc 3-5 $PID
   # Check actual CPU — task may still be on 0-2
   taskset -pc $PID
   cat /proc/$PID/stat | awk '{print $39}'
   ```

5. **What kSTEP would need**: To support this class of bugs, kSTEP would need:
   - A `kstep_cpu_hotplug(cpu, online)` API to bring CPUs online/offline
   - Support for kernels older than v5.15 (specifically v5.10-v5.13)
   - A `kstep_task_set_affinity(p, mask)` API that calls `set_cpus_allowed_ptr()` directly (rather than the current `kstep_task_pin()` which may use a different code path)
   The CPU hotplug requirement is a fundamental capability gap, not a minor extension, as it involves complex interactions with the stopper thread, CPU state machines, and scheduler balancing infrastructure.
