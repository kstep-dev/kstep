# Uclamp: task_fits_capacity() Ignores Uclamp Migration Margin and Capacity Pressure Rules

**Commit:** `b48e16a69792b5dc4a09d6807369d11b2970cc36`
**Affected files:** `kernel/sched/fair.c`, `kernel/sched/sched.h`
**Fixed in:** v6.2-rc1
**Buggy since:** v5.6-rc1 (commit `a7008c07a568` "sched/fair: Make task_fits_capacity() consider uclamp restrictions")

## Bug Description

The `task_fits_capacity()` function, which determines whether a task "fits" on a CPU based on its utilization and the CPU's capacity, does not correctly account for uclamp restrictions when interacting with the scheduler's 20% migration margin and capacity pressure. This function is used in three critical code paths: `update_misfit_status()` (misfit task detection), `detach_tasks()` in the `migrate_misfit` case (load balancing migration decisions), and `update_sg_wakeup_stats()` (wakeup-time group misfit classification).

The old `task_fits_capacity()` simply called `fits_capacity(uclamp_task_util(p), capacity)`, which clamps the task's estimated utilization between `uclamp_min` and `uclamp_max`, then compares the clamped value against the CPU's available capacity using the `fits_capacity()` macro. The `fits_capacity()` macro applies a 20% migration margin: `(cap) * 1280 < (max) * 1024`, meaning a task "fits" only if its utilization is below 80% of the capacity. This migration margin is designed to eagerly upmigrate tasks to higher-capacity CPUs before they are fully saturated.

However, when uclamp is active, applying the migration margin and capacity pressure to the clamped utilization produces incorrect results. For a task capped with `uclamp_max` equal to a CPU's original capacity, the clamped utilization at the cap level will always exceed 80% of that capacity, causing `fits_capacity()` to return false. This incorrectly marks the task as "not fitting" and triggers misfit migration to a larger CPU — directly contradicting the user's intent of capping the task to that performance level. Similarly, for boosted tasks, the old code applies capacity pressure (via `capacity_of()` rather than `capacity_orig_of()`) to the boosted comparison, which is wrong because temporary capacity pressure should not affect whether the CPU can originally satisfy the boost request.

The fix introduces a new `task_fits_cpu()` function (replacing `task_fits_capacity()`) that delegates to the newly introduced `util_fits_cpu()`, which properly separates the uclamp comparison from the migration margin and capacity pressure. Additionally, the `update_sg_wakeup_stats()` function is changed from checking against the group's maximum capacity (`group->sgc->max_capacity`) to checking per-CPU fitness, which is more accurate when individual CPUs within a group have different effective capacities due to thermal pressure.

## Root Cause

The root cause is that `task_fits_capacity()` conflates two conceptually distinct comparisons into a single `fits_capacity()` call. The function was:

```c
static inline int task_fits_capacity(struct task_struct *p,
                                     unsigned long capacity)
{
    return fits_capacity(uclamp_task_util(p), capacity);
}
```

Where `uclamp_task_util(p)` returns `clamp(task_util_est(p), uclamp_min, uclamp_max)`, and `fits_capacity(cap, max)` is defined as `(cap) * 1280 < (max) * 1024` (i.e., cap < 0.8 × max).

There are three problems with this approach:

**Problem 1: Migration margin applied to uclamp_max.** When a task is capped with `uclamp_max = C` (where C is the capacity of a CPU), the intent is that the task should be content to run on any CPU with `capacity_orig >= C`. But `fits_capacity()` applies the 20% margin, so `fits_capacity(C, C)` returns `C * 1280 < C * 1024` which is always false. This means a task capped to exactly a CPU's capacity is always considered "not fitting," triggering spurious misfit migration. For example, with `uclamp_max = 512` and a LITTLE CPU of `capacity_orig = 512`, `fits_capacity(512, 512)` = `655360 < 524288` = false.

**Problem 2: Capacity pressure applied to uclamp comparisons.** The `capacity` argument passed to `task_fits_capacity()` was `capacity_of(cpu)`, which subtracts RT/DL/IRQ pressure from the original capacity. But uclamp values represent user-specified performance levels that should be compared against the CPU's original capacity (`capacity_orig_of(cpu)`), not its currently available capacity. If a CPU has `capacity_orig = 1024` but `capacity_of = 900` due to some RT task pressure, a task with `uclamp_max = 1024` should still be considered fitting on that CPU. The old code would compare against 900, potentially producing wrong answers.

**Problem 3: Group-level misfit check in `update_sg_wakeup_stats()`.** The old code checked `!task_fits_capacity(p, group->sgc->max_capacity)` after the per-CPU loop. This used the group's maximum capacity, which is a static value that doesn't account for per-CPU variations in effective capacity (e.g., from thermal pressure). The new code checks `task_fits_cpu(p, i)` for each CPU `i` in the group during the per-CPU loop, clearing `group_misfit_task_load` if the task fits any CPU. This approach is initialized pessimistically (`sgs->group_misfit_task_load = 1` before the loop) and cleared if any CPU can accommodate the task.

The companion commit `48d5e9daa8b7` ("sched/uclamp: Fix relationship between uclamp and migration margin") introduced `util_fits_cpu()` which properly handles all three cases. `util_fits_cpu()` separates the comparison into:
- **Real util vs. capacity_of(cpu)**: Standard `fits_capacity()` with migration margin and pressure (for the actual workload).
- **uclamp_max vs. capacity_orig_of(cpu)**: No migration margin, no capacity pressure (except thermal for uclamp_min). If `uclamp_max <= capacity_orig`, the task is forced to fit.
- **uclamp_min vs. capacity_orig_thermal**: For boosted tasks, checks if the boost level fits the CPU's capacity minus thermal pressure, but without migration margin or other pressure.

## Consequence

The primary observable impact is **incorrect misfit task detection and migration behavior** on asymmetric CPU capacity systems (big.LITTLE, DynamIQ) when uclamp is used.

**Spurious misfit migration of capped tasks:** When a task has `uclamp_max` set to the capacity of a LITTLE or medium CPU, `update_misfit_status()` incorrectly sets `rq->misfit_task_load` to a non-zero value. This triggers misfit load balancing, which attempts to migrate the capped task to a higher-capacity (big) CPU. This directly contradicts the user's intent: they set `uclamp_max` to limit the task to a specific performance level, but the scheduler keeps trying to move it to a bigger CPU where the cap makes it equally unnecessary. The task may ping-pong between CPUs or consume unnecessary big-CPU time slots, increasing energy consumption on mobile/embedded devices where big.LITTLE is most common.

**Incorrect wake-up placement:** In `update_sg_wakeup_stats()`, the old code incorrectly classifies scheduling groups as having misfit tasks during wake-up balancing. This causes `find_idlest_group()` to prefer groups with higher capacity for tasks that are capped and should stay on lower-capacity groups. The per-group check using `group->sgc->max_capacity` also missed per-CPU capacity differences within a group (e.g., from thermal throttling).

**Incorrect migration decisions in load balancing:** In `detach_tasks()` with `migrate_misfit` type, the old code uses `task_fits_capacity(p, capacity_of(env->src_cpu))` to decide if a task is a misfit candidate for migration. The wrong answer here means either: (a) capped tasks are unnecessarily migrated away from their appropriate CPU, or (b) boosted tasks that should be migrated are not detected as misfit under certain capacity pressure conditions.

## Fix Summary

The fix replaces `task_fits_capacity()` with a new `task_fits_cpu()` function that takes a CPU number instead of a raw capacity value and internally calls `util_fits_cpu()`:

```c
static inline int task_fits_cpu(struct task_struct *p, int cpu)
{
    unsigned long uclamp_min = uclamp_eff_value(p, UCLAMP_MIN);
    unsigned long uclamp_max = uclamp_eff_value(p, UCLAMP_MAX);
    unsigned long util = task_util_est(p);
    return util_fits_cpu(util, uclamp_min, uclamp_max, cpu);
}
```

By taking a CPU number, `task_fits_cpu()` can internally call `capacity_of(cpu)`, `capacity_orig_of(cpu)`, and `arch_scale_thermal_pressure(cpu)` to make properly separated comparisons. The `util_fits_cpu()` function (introduced by the companion patch `48d5e9daa8b7`) compares the real utilization against `capacity_of(cpu)` with migration margin (standard behavior), but compares `uclamp_max` and `uclamp_min` against `capacity_orig_of(cpu)` without migration margin and without capacity pressure (except thermal for `uclamp_min`).

The three call sites are updated: `update_misfit_status()` changes from `task_fits_capacity(p, capacity_of(cpu_of(rq)))` to `task_fits_cpu(p, cpu_of(rq))`. The `detach_tasks()` migrate_misfit case changes from `task_fits_capacity(p, capacity_of(env->src_cpu))` to `task_fits_cpu(p, env->src_cpu)`. The `update_sg_wakeup_stats()` function is restructured: instead of checking `!task_fits_capacity(p, group->sgc->max_capacity)` after the loop, it initializes `sgs->group_misfit_task_load = 1` before the per-CPU loop (under `SD_ASYM_CPUCAPACITY`) and clears it to 0 inside the loop if `task_fits_cpu(p, i)` returns true for any CPU `i`. This per-CPU checking is both more correct (accounts for per-CPU thermal differences) and uses the proper uclamp-aware fitness check.

A stub `uclamp_eff_value()` is also added for the `!CONFIG_UCLAMP_TASK` case in `sched.h`, returning 0 for `UCLAMP_MIN` and `SCHED_CAPACITY_SCALE` for `UCLAMP_MAX`, so that `task_fits_cpu()` compiles without `CONFIG_UCLAMP_TASK` and degrades to the standard `fits_capacity()` behavior.

## Triggering Conditions

The bug requires all of the following conditions:

1. **Asymmetric CPU capacity system** (`SD_ASYM_CPUCAPACITY` flag set): The system must have CPUs with different capacities, such as a big.LITTLE ARM SoC. Without this, `update_misfit_status()` returns early (`!sched_asym_cpucap_active()`), and the `SD_ASYM_CPUCAPACITY` checks in `update_sg_wakeup_stats()` and `detach_tasks()` are skipped.

2. **CONFIG_UCLAMP_TASK enabled**: The bug only manifests when uclamp is active. Without uclamp, `uclamp_task_util(p)` returns `task_util_est(p)` and the behavior matches the non-uclamp case correctly. When uclamp is not compiled in, `util_fits_cpu()` returns the simple `fits_capacity()` result.

3. **Task with specific uclamp values**: The task must have uclamp settings that interact with the migration margin. The most reliable trigger is `uclamp_max = capacity_orig_of(some_cpu)`, which causes `fits_capacity(uclamp_max, capacity)` to always return false for that CPU due to the 20% margin. For example, `uclamp_max = 512` on a system where LITTLE CPUs have `capacity_orig = 512`.

4. **Task running on or being evaluated for a CPU whose capacity matches the uclamp constraint**: For the misfit bug, the task must be currently running on a CPU where `capacity_of(cpu) ≈ uclamp_max`, causing the migration margin comparison to fail. For the wakeup bug, the task is being placed and `update_sg_wakeup_stats()` evaluates groups.

5. **Task with `task_util_est > 80% of uclamp_max`**: The task's estimated utilization (after clamping) must exceed 80% of the CPU capacity for `fits_capacity()` to return false. With `uclamp_max = 512` and `task_util_est >= 410`, `fits_capacity(min(util, 512), 512)` will return false.

6. **Task must be allowed to run on multiple CPUs** (`p->nr_cpus_allowed > 1`): `update_misfit_status()` skips single-CPU-affinity tasks.

The bug is deterministic and reliably reproducible once these conditions are met. No race condition or specific timing is required — the wrong comparison fires every time `update_misfit_status()` or `update_sg_wakeup_stats()` is called with matching conditions.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by creating an asymmetric capacity topology with uclamp-capped tasks and observing incorrect `misfit_task_load` values.

### Step 1: Topology Setup

Configure a 2-CPU asymmetric system using kSTEP's topology and capacity APIs:
- CPU 0: Reserved for the driver (as per kSTEP constraints).
- CPU 1: LITTLE core with `capacity = 512`.
- CPU 2: BIG core with `capacity = 1024`.

Use `kstep_topo_init()`, `kstep_topo_set_mc()` to create a single cluster containing CPUs 1 and 2, then `kstep_topo_apply()`. Set capacities with `kstep_cpu_set_capacity(1, 512)` and `kstep_cpu_set_capacity(2, 1024)`. The QEMU instance must be configured with at least 3 CPUs.

### Step 2: Task Creation and Uclamp Configuration

Create a CFS task and set its `uclamp_max` to 512 (matching the LITTLE CPU's capacity) using `sched_setattr_nocheck()` as demonstrated in the existing `uclamp_inversion.c` driver:

```c
struct sched_attr attr = {
    .size = sizeof(attr),
    .sched_policy = SCHED_NORMAL,
    .sched_flags = SCHED_FLAG_UTIL_CLAMP,
    .sched_util_min = 0,
    .sched_util_max = 512,
    .sched_nice = 0,
};
sched_setattr_nocheck(p, &attr);
```

Pin the task to CPUs 1-2 using `kstep_task_pin(p, 1, 2)` so it can run on both LITTLE and BIG but not CPU 0.

### Step 3: Build Up Task Utilization

Wake the task on CPU 1 (the LITTLE core) and run ticks to let `task_util_est(p)` accumulate. The goal is to get `task_util_est(p)` above 410 (which is ~80% of 512). Since the task is running continuously, its utilization will ramp up over many ticks. Use `kstep_task_wakeup(p)` then `kstep_tick_repeat(N)` with enough ticks for PELT to ramp up the utilization.

After accumulation, the task's `se.avg.util_est` should be close to or above 410. The effective clamped utilization is `min(task_util_est, uclamp_max) = min(util, 512)`.

### Step 4: Observe Misfit Status

After each tick, in the `on_tick_begin` callback, read `rq->misfit_task_load` for CPU 1 via `cpu_rq(1)->misfit_task_load` (accessible through `internal.h`):

```c
struct rq *rq1 = cpu_rq(1);
unsigned long misfit = rq1->misfit_task_load;
```

Also read the task's effective uclamp values and estimated utilization:
```c
unsigned long util_est = READ_ONCE(p->se.avg.util_est.enqueued); // or task_util_est(p)
unsigned long uclamp_min = uclamp_eff_value(p, UCLAMP_MIN);
unsigned long uclamp_max = uclamp_eff_value(p, UCLAMP_MAX);
```

### Step 5: Pass/Fail Criteria

**On the buggy kernel (before fix):** Once `task_util_est(p) > ~410`, `update_misfit_status()` calls `task_fits_capacity(p, capacity_of(1))`. This evaluates `fits_capacity(uclamp_task_util(p), 512)` where `uclamp_task_util(p) = clamp(util, 0, 512) ≈ util` (capped at 512 at most). For util >= 410: `410 * 1280 = 524800 < 512 * 1024 = 524288` → false. At util=410, this is borderline; at util=411 it is clearly false. For util=512 (fully ramped and capped): `512 * 1280 = 655360 < 524288` → false. So `rq->misfit_task_load` will be non-zero. This is the bug: the task is capped to 512 and is on a CPU with `capacity_orig = 512` — it should fit, but the migration margin causes it to be flagged as misfit.

**On the fixed kernel (after fix):** `task_fits_cpu(p, 1)` calls `util_fits_cpu(util, 0, 512, 1)`. Inside, `fits = fits_capacity(util, capacity_of(1))` may still be false for high util. But then `uclamp_max_fits` is checked: `capacity_orig = capacity_orig_of(1) = 512`, so `(512 != 1024) && (512 <= 512) = true`. The final `fits = false || true = true`. So `rq->misfit_task_load` will be 0. The task is correctly recognized as fitting on CPU 1.

The driver should emit `kstep_pass()` if:
- On buggy kernel: `misfit_task_load != 0` when `task_util_est >= 410` and `uclamp_max == 512` on CPU 1 with `capacity_orig = 512` (demonstrates the bug).
- On fixed kernel: `misfit_task_load == 0` under the same conditions (demonstrates the fix).

Alternatively, a single driver can use `kstep_fail()` if misfit_task_load is non-zero when the task's uclamp_max equals the CPU's capacity_orig (which should never happen with a correct implementation), and `kstep_pass()` otherwise.

### Step 6: Guard with LINUX_VERSION_CODE

Guard the driver with `#if LINUX_VERSION_CODE` to target kernels in the range v5.6 to v6.1 (where the bug exists). The companion patch `48d5e9daa8b7` that introduces `util_fits_cpu()` is a prerequisite, so the buggy kernel must have the previous patch in the series (commit `a7008c07a568`) but not this fix. Use:
```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
```

### Step 7: Additional Verification (update_sg_wakeup_stats)

To verify the `update_sg_wakeup_stats()` fix, trigger a wakeup path that calls `find_idlest_group()`. This requires the task to wake up and the scheduler to evaluate scheduling groups. After blocking the task and waking it, observe whether the scheduler selects the correct CPU group. This is harder to directly observe from a driver, but the misfit_task_load check on the tick path is the clearest signal.

### Required kSTEP Features

All required features exist in kSTEP:
- `kstep_cpu_set_capacity()` for asymmetric capacity
- `kstep_topo_*` for topology setup
- `sched_setattr_nocheck()` for uclamp (kernel function, used in existing uclamp_inversion.c)
- `cpu_rq()` and internal scheduler structures via `internal.h`
- `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_wakeup()`, `kstep_tick_repeat()`

No kSTEP extensions are needed. CONFIG_UCLAMP_TASK must be enabled in the kernel config.
