# Core: Redundant Task Migration in migration_cpu_stop() When Already on Valid CPU

**Commit:** `3f1bc119cd7fc987c8ed25ffb717f99403bb308c`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.12-rc3
**Buggy since:** v5.11-rc1 (introduced by `6d337eab041d` "sched: Fix migrate_disable() vs set_cpus_allowed_ptr()")

## Bug Description

The `migration_cpu_stop()` function is the stopper callback used to perform CPU migration of tasks. It is invoked in two modes: (1) with a specific `dest_cpu >= 0` for targeted migration (e.g., from `sched_exec()` or `migrate_task_to()`), and (2) with `dest_cpu < 0` meaning "migrate to any valid CPU in the task's cpus_mask" (used by `affine_move_task()` when the current CPU was removed from the allowed set).

When called with `dest_cpu < 0`, the buggy code unconditionally calls `cpumask_any_distribute(&p->cpus_mask)` to pick a new destination CPU and then performs the migration. It does not first check whether the task is already running on a CPU that is within its allowed mask (`p->cpus_mask`). This means that even if the task ended up on a valid CPU by the time the stopper runs (e.g., due to a concurrent load balance or another migration), it will be unnecessarily migrated to a different CPU.

This is especially problematic in the `affine_move_task()` path where `set_cpus_allowed_ptr()` changes the task's affinity. If between the affinity change and the stopper execution the task naturally migrates to a valid CPU (via load balancing, wakeup placement, or another concurrent `set_cpus_allowed_ptr()`), the stopper should recognize this and skip the redundant migration. Instead, it always picks a new CPU and moves the task there, causing unnecessary cache cold misses, runqueue lock contention, and wasted stopper thread cycles.

The bug was introduced by commit `6d337eab041d` ("sched: Fix migrate_disable() vs set_cpus_allowed_ptr()") which reworked the `migration_cpu_stop()` / `affine_move_task()` interaction in v5.11-rc1. That commit restructured the migration logic for `migrate_disable()` support but failed to include the optimization of checking whether the task was already on a valid CPU before proceeding with migration.

## Root Cause

In `migration_cpu_stop()`, when the function receives `dest_cpu < 0` (indicating "any valid CPU"), the code path before the fix was:

```c
if (dest_cpu < 0)
    dest_cpu = cpumask_any_distribute(&p->cpus_mask);

if (task_on_rq_queued(p))
    rq = __migrate_task(rq, &rf, p, dest_cpu);
else
    p->wake_cpu = dest_cpu;
```

The function unconditionally selects a new destination via `cpumask_any_distribute()` and then calls `__migrate_task()` to move the task there. There is no early-exit check to see if `task_cpu(p)` is already a member of `p->cpus_mask`.

The root cause is a missing short-circuit check. The intent of `dest_cpu < 0` is "make sure the task is on a valid CPU," not "always move the task to a different CPU." The correct behavior should be: if the task is already on a CPU that is within its allowed mask, the migration goal has already been satisfied and no actual migration is needed.

This was a latent issue from the refactoring in `6d337eab041d`. The `else if (pending)` branch (lines 1962-1977 in the pre-fix code) already had a similar check: `if (cpumask_test_cpu(task_cpu(p), p->cpus_ptr)) { ... goto out; }` — recognizing that if the task moved to a valid CPU before the stopper ran, no further action was needed. However, the primary `if (task_rq(p) == rq)` branch (where the task is still on the expected rq) was missing this equivalent optimization.

The consequence is that every time `affine_move_task()` queues a stopper to move a task to "any" valid CPU, even if the task races to a valid CPU before the stopper executes (or the task's CPU was re-added to the allowed mask by another concurrent `set_cpus_allowed_ptr()`), the migration will proceed unnecessarily.

## Consequence

The primary consequence is unnecessary task migrations. When `migration_cpu_stop()` runs with `dest_cpu < 0` and the task is already on a valid CPU, it will:

1. Select a potentially different CPU via `cpumask_any_distribute()`.
2. Call `__migrate_task()` which dequeues the task from its current runqueue and enqueues it on the destination runqueue, acquiring locks on both runqueues.
3. The task loses CPU cache warmth, causing performance degradation when it resumes execution on the new CPU.

This is classified as a performance bug rather than a correctness/crash bug. There are no kernel panics, oopses, or data corruption. However, unnecessary migrations cause measurable overhead: increased lock contention on runqueue locks, cache misses from cold caches on the new CPU, and wasted stopper thread execution time. In workloads with frequent affinity changes (e.g., irqbalance, container migration, NUMA balancing interactions), the unnecessary migrations can accumulate and cause noticeable throughput degradation.

Additionally, the commit message and the appended XXX comment note that `__migrate_task()` can fail (the destination CPU might have gone offline between selection and execution), in which case the task could remain on a "dodgy" CPU. While this scenario is primarily a CPU hotplug concern, the unnecessary migration attempt increases the window for such failure.

## Fix Summary

The fix adds an early-exit check inside the `dest_cpu < 0` code path within `migration_cpu_stop()`. Before selecting a new destination CPU, the code now checks whether the task's current CPU (`task_cpu(p)`) is already within the task's allowed CPU mask (`p->cpus_mask`):

```c
if (dest_cpu < 0) {
    if (cpumask_test_cpu(task_cpu(p), &p->cpus_mask))
        goto out;

    dest_cpu = cpumask_any_distribute(&p->cpus_mask);
}
```

If `task_cpu(p)` is already in `p->cpus_mask`, the function jumps to the `out` label, skipping the migration entirely. This is correct because the purpose of `dest_cpu < 0` is to ensure the task runs on a valid CPU — if it already is, the goal is satisfied.

The fix also adds a comment acknowledging that `__migrate_task()` can fail (when the selected `dest_cpu` goes offline between selection and the actual migration), noting that this can only happen during CPU hotplug and the task will be pushed out anyway. This is a defensive note rather than a code change.

This optimization mirrors the logic already present in the `else if (pending)` branch, which checks `cpumask_test_cpu(task_cpu(p), p->cpus_ptr)` and bails out early if the task has already reached a valid CPU. The fix brings the same optimization to the primary path where `task_rq(p) == rq`.

## Triggering Conditions

The bug triggers under the following conditions:

1. **Kernel version**: v5.11-rc1 through v5.12-rc2 (after `6d337eab041d` introduced `migrate_disable()` rework and before the fix).
2. **CPU count**: At least 2 CPUs are required for migration to be meaningful.
3. **Affinity change with `dest_cpu < 0`**: The `affine_move_task()` function must be invoked with migration pending, which happens when `set_cpus_allowed_ptr()` changes a task's affinity while the task is running or migrate-disabled. This results in `migration_cpu_stop()` being called with `dest_cpu == -1` (meaning "any valid CPU").
4. **Task already on valid CPU**: Between the time the stopper work is queued and the time `migration_cpu_stop()` executes, the task must end up on a CPU that is within its new allowed mask. This can happen via: (a) concurrent load balancing moving the task, (b) the task waking up on a valid CPU, (c) another concurrent `set_cpus_allowed_ptr()` restoring the current CPU to the allowed mask, or (d) the current CPU being in both the old and new allowed masks (but migration was triggered because `migrate_disable()` was active during the affinity change).
5. **No special kernel configuration** required beyond `CONFIG_SMP=y`.

The bug is deterministic in the sense that any `migration_cpu_stop()` call with `dest_cpu < 0` where the task is already on a valid CPU will perform an unnecessary migration. The probability depends on workload characteristics — it is most likely when there are frequent affinity changes and concurrent load balancing, or when `migrate_disable()` / `migrate_enable()` sequences overlap with `set_cpus_allowed_ptr()`.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why this bug cannot be reproduced with kSTEP

**Kernel version too old.** The bug was introduced in v5.11-rc1 by commit `6d337eab041d` and fixed in v5.12-rc3 by commit `3f1bc119cd7fc987c8ed25ffb717f99403bb308c`. kSTEP supports Linux v5.15 and newer only. Since the fix was merged well before v5.15, any v5.15+ kernel already contains this fix. There is no kernel version within kSTEP's supported range where this bug exists.

### 2. What would need to be added to kSTEP

Even if the kernel version were supported, reproducing this bug would require:

- **`sched_setaffinity()` or `set_cpus_allowed_ptr()` API**: kSTEP would need a `kstep_task_setaffinity(p, mask)` function that calls `set_cpus_allowed_ptr()` to change a task's affinity mask at runtime, triggering the `affine_move_task()` path. Currently, kSTEP only has `kstep_task_pin(p, begin, end)` which sets initial CPU affinity but does not change it on a running task in a way that exercises the stopper-based migration path.
- **`migrate_disable()` / `migrate_enable()` support**: The bug is specifically related to the interaction between `migrate_disable()` and `set_cpus_allowed_ptr()`. kSTEP would need wrappers to invoke these functions on kSTEP-managed tasks.
- **Observation of migration events**: kSTEP would need a callback or hook in `migration_cpu_stop()` to detect whether an unnecessary migration occurred (i.e., the task was moved even though it was already on a valid CPU).

### 3. Version constraint

This is primarily a **kernel version too old** case. The fix is in v5.12-rc3, and kSTEP's minimum supported version is v5.15. All v5.15+ kernels include this fix.

### 4. Alternative reproduction methods

Outside kSTEP, this bug could be reproduced on a v5.11 or v5.12-rc1/rc2 kernel by:

1. Running a multi-threaded workload on a multi-CPU system.
2. Using `sched_setaffinity()` from userspace to repeatedly change a task's CPU affinity while the task is running.
3. Observing (via ftrace `sched_migrate_task` tracepoint) that unnecessary migrations occur — specifically, migrations where the source CPU was already in the new affinity mask.
4. Comparing migration counts between buggy (v5.11) and fixed (v5.12-rc3+) kernels under the same workload.

Alternatively, a kernel module on a v5.11 kernel could:
1. Create kthreads pinned to specific CPUs.
2. Call `set_cpus_allowed_ptr()` to change affinity while the kthread is running.
3. Use tracepoints or direct instrumentation of `migration_cpu_stop()` to detect that the task was migrated even though it was already on a valid CPU.
