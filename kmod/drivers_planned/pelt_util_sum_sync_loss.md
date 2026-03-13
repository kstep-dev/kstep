# PELT: util_sum Hard Sync Causes Excessive Utilization Loss

**Commit:** `98b0d890220d45418cfbc5157b3382e6da5a12ab`
**Affected files:** kernel/sched/fair.c, kernel/sched/pelt.h
**Fixed in:** v5.17-rc2
**Buggy since:** v5.14-rc1 (commit 1c35b07e6d39 "sched/fair: Ensure _sum and _avg values stay consistent")

## Bug Description

In the Linux kernel's PELT (Per-Entity Load Tracking) subsystem, the function `update_cfs_rq_load_avg()` in `kernel/sched/fair.c` processes removed task contributions when tasks migrate away from a CPU or are dequeued from a cfs_rq. Commit 1c35b07e6d39 introduced a "hard sync" that resets `util_sum` to exactly `util_avg * divider` after subtracting removed utilization contributions. This hard sync was intended to ensure consistency between `_sum` and `_avg` values and prevent a `SCHED_WARN_ON()` in `cfs_rq_is_decayed()` from triggering. However, it inadvertently causes a significant and systematic loss of accumulated utilization tracking data.

The PELT divider is defined as `LOAD_AVG_MAX - 1024 + period_contrib`, where `period_contrib` represents the partial contribution within the current 1024µs PELT period. The relationship between `_sum` and `_avg` values is: `_avg ≈ _sum / divider`. However, `_sum` naturally accumulates finer-grained information than `_avg` — specifically, up to `LOAD_AVG_MAX - 1` (approximately 47741) units of contribution can be present in `_sum` that have not yet been reflected in `_avg`. When the hard sync forcibly sets `util_sum = util_avg * divider`, this unreflected contribution is thrown away.

This loss is compounded when cgroup hierarchies are involved. The `update_tg_cfs_util()` function propagates utilization changes from a group's cfs_rq to its parent's sched_entity and then to the parent's cfs_rq. At each level of the hierarchy, the artificially lowered `util_sum` produces a correspondingly lower `util_avg`, which then propagates downward. On real-world systems with task groups (e.g., Android's `/foreground`, `/background`, `/system` cgroups or container cgroups like `/A/B`), the cumulative effect across multiple cgroup levels can reduce the root cfs_rq's `util_avg` by hundreds or even thousands of units, as demonstrated in the bugzilla discussion (kernel bugzilla #215045) where differences of up to 8000+ util_avg were observed between buggy and reverted kernels.

The practical consequence reported by Google engineers was a ~10% frame rate drop in video recording on ARM big.LITTLE devices, caused by schedutil selecting lower CPU frequencies due to the artificially suppressed utilization values. The bug was reliably reproduced on both ARM Juno development boards and production Android devices.

## Root Cause

The root cause lies in the `update_cfs_rq_load_avg()` function in `kernel/sched/fair.c`, specifically in the code path that handles `cfs_rq->removed.nr > 0`. When tasks are dequeued from a cfs_rq (e.g., due to migration), their PELT contributions are added to `cfs_rq->removed.{util_avg, load_avg, runnable_avg}`. These accumulated removals are processed in bulk the next time `update_cfs_rq_load_avg()` is called.

The buggy code (introduced by commit 1c35b07e6d39) performs the following sequence:

```c
r = removed_util;
sub_positive(&sa->util_avg, r);
sa->util_sum = sa->util_avg * divider;  /* BUG: hard sync */
```

The `divider` is computed as `get_pelt_divider(&cfs_rq->avg)`, which returns `LOAD_AVG_MAX - 1024 + period_contrib`. The problem is that `util_sum` naturally contains more information than `util_avg * divider`. Between periodic PELT updates (which happen roughly every 1ms via `___update_load_avg()`), the `accumulate_sum()` function increments `util_sum` based on the actual running time within partial PELT periods. This accumulated-but-not-yet-reflected contribution can be up to `LOAD_AVG_MAX - 1` (47741) in magnitude. The hard sync `util_sum = util_avg * divider` discards this entire amount.

The discarded contribution corresponds to `util_avg` of up to approximately 22 units (`(LOAD_AVG_MAX - 1) >> SCHED_CAPACITY_SHIFT ≈ 47741 / 2048 ≈ 23`) per removal processing. While 22 might seem small, the effect is multiplicative across cgroup hierarchy levels. Consider a scenario with nested cgroups `/A/B`:

1. When `update_cfs_rq_load_avg()` processes removals on the `/A/B` cfs_rq, `util_sum` (and consequently `util_avg`) drops by up to 22 more than it should.
2. The `update_tg_cfs_util()` function then propagates this to the sched_entity for `/A/B` in `/A`'s cfs_rq, again hard-syncing `se->avg.util_sum = se->avg.util_avg * divider`.
3. The delta propagated to `/A`'s cfs_rq is larger than it should be, and `/A`'s own `util_sum` gets hard-synced as well.
4. This cascades up to the root cfs_rq.

Furthermore, when `update_cfs_rq_load_avg()` is called frequently (which happens on every `update_load_avg()` call during enqueue/dequeue, not just from `update_blocked_averages()`), each call can trigger the hard sync and lose accumulated contribution, even if `cfs_rq->removed.nr` is only incremented by a single task.

A secondary issue discovered during the investigation is that due to rounding, a sched_entity's `util_sum` can end up being +1 more than the cfs_rq's `util_sum`. When many tasks are detached between two periodic PELT updates (~1ms), each contributing a +1 rounding error to the subtraction, the cumulative effect can drive `cfs_rq->util_sum` to zero while `util_avg` remains non-zero. This is the additional problem the fix addresses with the `max_t()` lower-bound check.

## Consequence

The primary observable consequence is significantly reduced CPU utilization tracking values (`util_avg`) at the root cfs_rq level, which directly impacts CPU frequency selection via the schedutil cpufreq governor. The schedutil governor uses `cpu_util_cfs()` to read the root cfs_rq's `util_avg` as the basis for choosing CPU operating frequency. With the buggy hard sync, this value is systematically lower than it should be, causing schedutil to select lower frequencies than the workload demands.

On ARM big.LITTLE systems running Android, Google reported a ~10% frame rate drop in 1080p 60fps video recording (from ~55 FPS to ~47 FPS non-error). Instrumented traces showed `util_diff` (difference between buggy and correct `util_avg`) values of up to 1368 on individual CPUs, with `util_diff_max` values reaching 24534. On systems using Android vendor hooks, the differences were even more extreme, reaching `util_diff_max` of 94174. Dietmar Eggemann confirmed similar behavior on ARM Juno-r0 boards running `hackbench`, observing `util_diff_max` up to 20000 and actual `util_avg` differences of 8000+ between buggy and correct implementations.

Beyond performance degradation, a secondary consequence is that the hard sync can also cause `util_sum` to reach zero while `util_avg` remains positive, triggering a `SCHED_WARN_ON()` in `cfs_rq_is_decayed()`. This was observed when running the original commit 1c35b07e6d39 was reverted — the warn could trigger in nested cgroup setups with heavy task migration (e.g., `hackbench` running in `/A/B`, `/C/D`, `/E/F` cgroups). The fix addresses both issues: it prevents the excessive utilization loss while also ensuring the `_sum`/`_avg` consistency that the original commit was trying to achieve.

## Fix Summary

The fix changes the removal handling in `update_cfs_rq_load_avg()` from a hard sync to a subtraction with a lower-bound guard. Instead of:

```c
sub_positive(&sa->util_avg, r);
sa->util_sum = sa->util_avg * divider;
```

The fix applies:

```c
sub_positive(&sa->util_avg, r);
sub_positive(&sa->util_sum, r * divider);
sa->util_sum = max_t(u32, sa->util_sum, sa->util_avg * PELT_MIN_DIVIDER);
```

The first change (`sub_positive` instead of hard assignment) preserves the accumulated contribution in `util_sum` that has not yet been reflected in `util_avg`. It subtracts only the proportional amount that the removed task(s) contributed, rather than resetting the entire sum. This prevents the systematic loss of up to `LOAD_AVG_MAX - 1` units of accumulated data per removal processing.

The second change (the `max_t` lower-bound check) addresses the rounding issue where individual sched_entity `util_sum` values can be +1 above the cfs_rq's `util_sum`. The constant `PELT_MIN_DIVIDER` is defined as `LOAD_AVG_MAX - 1024` (the minimum possible divider value, when `period_contrib` is 0). By ensuring `util_sum >= util_avg * PELT_MIN_DIVIDER`, the fix guarantees that `util_sum` never drops below what is mathematically consistent with `util_avg`, even after many rapid detachments with rounding errors accumulate. This prevents the `SCHED_WARN_ON()` in `cfs_rq_is_decayed()` from triggering.

Additionally, the fix introduces the `PELT_MIN_DIVIDER` macro in `kernel/sched/pelt.h` and refactors `get_pelt_divider()` to use it, replacing the open-coded `LOAD_AVG_MAX - 1024` expression. This improves code clarity and ensures consistency between the divider computation and the lower-bound check.

## Triggering Conditions

The bug requires the following conditions:

- **Kernel version**: v5.14-rc1 through v5.16 (any kernel containing commit 1c35b07e6d39 but not the fix).
- **CONFIG_FAIR_GROUP_SCHED=y**: The bug is most impactful with cgroup-based task grouping enabled, as the utilization loss is amplified through the cgroup hierarchy propagation. However, the hard sync also occurs at the root cfs_rq level even without cgroups.
- **CONFIG_SMP=y**: The removal path in `update_cfs_rq_load_avg()` processes `cfs_rq->removed`, which is populated when tasks migrate between CPUs. On UP systems, there is no migration.
- **Multiple CPUs**: At least 2 CPUs are needed to trigger task migration. The bug effect is proportional to migration frequency.
- **Cgroup hierarchy depth**: Deeper cgroup nesting amplifies the loss. The bugzilla discussion showed that even 2 levels of nesting (e.g., `/A/B`) produce significant effects. The Juno-r0 reproducer used 3 pairs of nested cgroups (`/A/B`, `/C/D`, `/E/F`).
- **Task migration frequency**: The bug triggers every time `update_cfs_rq_load_avg()` processes `cfs_rq->removed.nr > 0`. Higher migration rates mean more frequent hard syncs and more accumulated loss.
- **Task count**: More tasks means more migrations and more removal events. The bugzilla reproducer used `hackbench` with 40 groups (1600 tasks).
- **Non-zero util_sum accumulation between PELT updates**: The loss is proportional to the amount of `util_sum` accumulated since the last periodic PELT update. Tasks that are actively running (contributing to `util_sum` between updates) will show the largest effect.

The bug is deterministic — it occurs on every invocation of `update_cfs_rq_load_avg()` that processes removals. There is no race condition or timing dependency. The only requirement is that tasks migrate between CPUs while belonging to cgroup hierarchies, and that `update_cfs_rq_load_avg()` is called between migration events.

For reliable observation, the workload should generate sustained CPU utilization (so `util_avg` and `util_sum` are high) followed by migration events. A workload like `hackbench` in nested cgroups is ideal because it creates many tasks that frequently migrate.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP by creating a cgroup hierarchy with CFS tasks, building up PELT utilization, then triggering task migration to cause the removal processing path, and observing the `util_sum` / `util_avg` values.

### Step 1: Topology and Configuration

Configure QEMU with at least 3 CPUs. No special topology is required — the default flat topology suffices. No frequency scaling or capacity asymmetry is needed since the bug is in the core PELT arithmetic, not in schedutil or EAS.

### Step 2: Create Cgroup Hierarchy

Create a nested cgroup hierarchy to amplify the propagation effect:
```c
kstep_cgroup_create("A");
kstep_cgroup_create("A/B");
```
This creates a 2-level hierarchy. The bug's util_sum loss at the `/A/B` cfs_rq level will propagate through the `/A` group entity to the root cfs_rq.

### Step 3: Create Tasks and Build Utilization

Create 8-10 CFS tasks, pin them all to CPU 1 (not CPU 0 which is reserved), and add them to the `/A/B` cgroup:
```c
struct task_struct *tasks[10];
for (int i = 0; i < 10; i++) {
    tasks[i] = kstep_task_create();
    kstep_task_pin(tasks[i], 1, 1);
    kstep_cgroup_add_task("A/B", tasks[i]->pid);
    kstep_task_wakeup(tasks[i]);
}
```

Then run many ticks to build up PELT values. PELT has a half-life of ~32ms, so approximately 500-1000 ticks (at 1ms tick interval) will saturate the utilization:
```c
kstep_tick_repeat(1000);
```

### Step 4: Record Baseline PELT Values

Use `KSYM_IMPORT` and internal.h access to read the cfs_rq's PELT values before triggering the bug. Access the task_group's cfs_rq for CPU 1:
```c
struct rq *rq1 = cpu_rq(1);
struct cfs_rq *root_cfs = &rq1->cfs;
```

For the `/A/B` cfs_rq, use the task_group pointer obtained via a task's `sched_task_group` field or by traversing the cgroup hierarchy. Record:
- `root_cfs->avg.util_avg` (root cfs_rq utilization average)
- `root_cfs->avg.util_sum` (root cfs_rq utilization sum)
- The ratio `util_sum / (util_avg * get_pelt_divider(&root_cfs->avg))`

### Step 5: Trigger Task Migration (Removal Processing)

Migrate all tasks from CPU 1 to CPU 2 by re-pinning them:
```c
for (int i = 0; i < 10; i++) {
    kstep_task_pin(tasks[i], 2, 2);
}
```

This will cause the tasks to be dequeued from CPU 1's cfs_rq hierarchy, populating `cfs_rq->removed.{nr, util_avg, ...}` for each cfs_rq in the hierarchy on CPU 1.

### Step 6: Trigger Removal Processing

Run a single tick on CPU 1 (or trigger `update_cfs_rq_load_avg()` via any scheduling event):
```c
kstep_tick();
```

This calls `update_load_avg()` → `update_cfs_rq_load_avg()`, which processes the `cfs_rq->removed` entries and applies the buggy hard sync.

### Step 7: Observe and Compare

Read the PELT values again after the removal processing:
```c
u32 util_avg_after = root_cfs->avg.util_avg;
u32 util_sum_after = root_cfs->avg.util_sum;
u32 divider = get_pelt_divider(&root_cfs->avg);
u32 expected_min_sum = util_avg_after * divider;
```

**Detection criteria:**

On the **buggy kernel**, after processing removals, `util_sum` will be exactly equal to `util_avg * divider` (the hard sync). The ratio `util_sum / (util_avg * divider)` will be exactly 1.0. More importantly, the `util_avg` value itself will be significantly lower than expected because the hard sync threw away accumulated contribution.

On the **fixed kernel**, `util_sum` will be greater than or equal to `util_avg * PELT_MIN_DIVIDER` but generally higher than `util_avg * divider`, because the subtraction-based approach preserves accumulated contribution.

The concrete pass/fail check:
```c
// Record util_avg before migration
u32 util_avg_before = root_cfs->avg.util_avg;
// ... do migration and tick ...
u32 util_avg_after = root_cfs->avg.util_avg;

// On buggy kernel: util_avg_after will be much lower
// On fixed kernel: util_avg_after will be closer to expected
// The difference util_avg_before - util_avg_after should approximately
// equal the sum of the migrated tasks' util_avg values.
// On buggy kernel it will be significantly MORE than that sum.

u32 task_util_sum = 0;
for (int i = 0; i < 10; i++)
    task_util_sum += tasks[i]->se.avg.util_avg;

s32 excess_loss = (util_avg_before - util_avg_after) - task_util_sum;
if (excess_loss > 50) {  // threshold for the rounding loss
    kstep_fail("Excessive util_avg loss: before=%u after=%u tasks=%u excess=%d",
               util_avg_before, util_avg_after, task_util_sum, excess_loss);
} else {
    kstep_pass("util_avg loss within expected range: excess=%d", excess_loss);
}
```

Alternatively, a simpler approach is to directly check whether `util_sum == util_avg * divider` after processing removals (which would indicate the hard sync occurred):
```c
u32 divider = get_pelt_divider(&root_cfs->avg);
if (root_cfs->avg.util_sum == root_cfs->avg.util_avg * divider) {
    kstep_fail("Hard sync detected: util_sum=%u == util_avg*divider=%u",
               root_cfs->avg.util_sum, root_cfs->avg.util_avg * divider);
} else {
    kstep_pass("util_sum preserved: util_sum=%u, util_avg*divider=%u",
               root_cfs->avg.util_sum, root_cfs->avg.util_avg * divider);
}
```

### Step 8: Use Callbacks for Fine-Grained Observation

For more precise observation, use the `on_tick_begin` / `on_tick_end` callbacks to log PELT values on every tick. This allows tracking the evolution of `util_sum` and `util_avg` across the migration event. Log the values using `kstep_json_field_u64()` for structured output.

### Expected Behavior

- **Buggy kernel (v5.14-rc1 to v5.16)**: After migration and tick, `root_cfs->avg.util_avg` will drop by significantly more than the sum of migrated tasks' `util_avg` values. The `util_sum` will be hard-synced to `util_avg * divider`. With 10 tasks in a 2-level cgroup hierarchy, the excess loss should be observable (tens to hundreds of `util_avg` units).

- **Fixed kernel (v5.17-rc2+)**: After migration and tick, `root_cfs->avg.util_avg` will drop by approximately the sum of migrated tasks' `util_avg` values (with small rounding error). The `util_sum` will be higher than `util_avg * divider`, preserving accumulated contribution. The `max_t` guard ensures `util_sum >= util_avg * PELT_MIN_DIVIDER`.

### kSTEP Requirements

This bug can be reproduced with existing kSTEP APIs:
- `kstep_cgroup_create()` for nested cgroup hierarchy
- `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_wakeup()` for task management
- `kstep_cgroup_add_task()` to assign tasks to cgroups
- `kstep_tick_repeat()` to build PELT values
- `internal.h` access for reading `cfs_rq->avg.util_avg` and `util_sum`
- `KSYM_IMPORT` if needed for `get_pelt_divider()` or equivalent computation

No extensions to kSTEP are required. The `PELT_MIN_DIVIDER` constant and `get_pelt_divider()` inline function are available through `kernel/sched/pelt.h` included via `internal.h`.
