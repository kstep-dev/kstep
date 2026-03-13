# Fair: Migrated Entity Vruntime Incorrectly Reset by Long Sleeper Detection

**Commit:** `a53ce18cacb477dd0513c607f187d16f0fa96f71`
**Affected files:** kernel/sched/fair.c, kernel/sched/core.c
**Fixed in:** v6.3-rc4
**Buggy since:** v6.3-rc1 (introduced by commit `829c1651e9c4` "sched/fair: sanitize vruntime of entity being placed")

## Bug Description

Commit `829c1651e9c4` introduced a safety mechanism in `place_entity()` to detect scheduling entities that have slept for an extremely long time. When a CFS entity sleeps so long that its vruntime falls very far behind `cfs_rq->min_vruntime`, the signed 64-bit comparison in `max_vruntime()` can overflow and produce an inverted result. The original fix computed `sleep_time = rq_clock_task() - se->exec_start` and, if this exceeded 60 seconds, bypassed the `max_vruntime()` comparison entirely, resetting the entity's vruntime to `cfs_rq->min_vruntime` (adjusted with the sleeper credit).

However, this fix ignored a critical interaction with the task migration path. When a CFS task migrates between CPUs, `migrate_task_rq_fair()` resets `se->exec_start = 0` to mark the entity as "no longer cache-hot" on its old CPU. This reset happens **before** the entity reaches `place_entity()` during enqueue on the new CPU. Consequently, when `place_entity()` computes `sleep_time = rq_clock_task() - 0 = rq_clock_task()`, it gets the entire wall-clock uptime of the runqueue as the "sleep time." On any system that has been up for more than 60 seconds, this always exceeds the 60-second threshold, causing **every migrated CFS entity** to be incorrectly treated as a long sleeper.

This means that after migration, the entity's vruntime is unconditionally reset to `cfs_rq->min_vruntime - sleeper_credit`, regardless of the entity's actual vruntime or how long it actually slept. This breaks CFS fairness for all migrated tasks on systems with >60 seconds of uptime, which is essentially all production systems.

The bug was reported by Zhang Qiao at Huawei and fixed by Vincent Guittot. The final upstream fix takes a different approach than the initial v2 patch proposed by Zhang Qiao: rather than fixing the long sleeper detection in `migrate_task_rq_fair()`, the fix delays the `exec_start` reset until after `place_entity()` completes, and introduces a new `entity_is_long_sleeper()` helper with a much more robust threshold calculation.

## Root Cause

The root cause is a temporal ordering problem between two operations: the `exec_start` reset in `migrate_task_rq_fair()` and the long sleeper detection in `place_entity()`.

In the buggy code, the migration path in `migrate_task_rq_fair()` (at the end of the function) contained:

```c
/* We have migrated, no longer consider this task hot */
se->exec_start = 0;
```

This reset serves the purpose of preventing the `task_hot()` check from considering the migrated task as cache-hot on its new CPU (since `exec_start` is used to compute how recently the task ran). However, `exec_start` is also used by `place_entity()` to estimate how long the entity has been sleeping:

```c
sleep_time = rq_clock_task(rq_of(cfs_rq)) - se->exec_start;
if ((s64)sleep_time > 60LL * NSEC_PER_SEC)
    se->vruntime = vruntime;
else
    se->vruntime = max_vruntime(se->vruntime, vruntime);
```

When a blocked task wakes up and migrates (the `TASK_WAKING` path), the execution order is:

1. `try_to_wake_up()` calls `select_task_rq_fair()` which selects a new CPU
2. `migrate_task_rq_fair()` is called: `se->vruntime -= old_min_vruntime` and `se->exec_start = 0`
3. `activate_task()` → `enqueue_task()` → `enqueue_entity()` is called on the new CPU
4. In `enqueue_entity()`: `se->vruntime += new_min_vruntime`, then `place_entity()` is called
5. `place_entity()` computes `sleep_time = rq_clock_task() - 0 = rq_clock_task()`
6. Since `rq_clock_task()` exceeds 60 billion nanoseconds (60 seconds) on any system up >1 minute, the long sleeper path always triggers

The result is that `se->vruntime` is unconditionally set to `cfs_rq->min_vruntime - thresh`, ignoring the entity's actual vruntime. The `max_vruntime()` comparison, which is essential for preventing a task from gaining unfair scheduling advantage by being "placed backwards," is completely bypassed.

Additionally, there is a secondary issue with the 60-second threshold itself. The comment in commit `829c1651e9c4` chose 60 seconds somewhat arbitrarily as "much longer than the characteristic scheduler time scale." But the actual maximum vruntime speedup relative to real time is determined by the ratio `scale_load_down(NICE_0_LOAD) / MIN_SHARES`. A more theoretically sound threshold, as adopted by the fix, is `2^63 / scale_load_down(NICE_0_LOAD)`, which corresponds to approximately 104 days—the point at which the vruntime difference could actually overflow a signed 64-bit integer.

A third issue involves clock_task divergence between CPUs. Because `rq_clock_task` on two different runqueues can differ due to IRQ time accounting and stolen time (in virtualized environments), a migrated entity's `exec_start` from its old CPU may not be directly comparable with `rq_clock_task` on the new CPU. The fix's increased threshold (~104 days) provides sufficient margin to absorb any such divergence.

## Consequence

The primary consequence is a **systematic violation of CFS fairness for all migrated tasks** on systems that have been running for more than 60 seconds. Every time a CFS task migrates to a new CPU (whether due to load balancing, affinity changes, or wakeup selection), its accumulated vruntime is discarded and replaced with `cfs_rq->min_vruntime - sleeper_credit`.

This has two distinct failure modes depending on the relative vruntime of the migrating task:

1. **Unfair scheduling boost**: If the migrating task had a vruntime significantly ahead of `min_vruntime` on the destination CPU (meaning it had consumed more than its fair share of CPU time recently), its vruntime is reset backwards to `min_vruntime - thresh`. This gives the task an undeserved scheduling credit, allowing it to monopolize the CPU at the expense of other tasks until it catches back up. This is the more common and impactful scenario, as tasks that have been running recently (and thus have high vruntime) will get a fresh start after every migration.

2. **Correct but unnecessary override**: If the migrating task's vruntime was already near or behind `min_vruntime`, the reset has minimal effect since `max_vruntime()` would have selected a similar value anyway. However, the sleeper credit subtraction means the task is always placed slightly behind `min_vruntime`, which is technically the correct behavior for a genuinely long-sleeping task but inappropriate for a recently-running migrated task.

On busy multi-CPU systems with frequent load balancing, this bug causes significant fairness degradation. Tasks that are frequently migrated (common in workloads with many short-lived or intermittently-active tasks) receive repeated vruntime resets, effectively starving tasks that remain on a single CPU. There are no kernel crashes, oopses, or warnings associated with this bug—it manifests purely as scheduling unfairness, making it difficult to detect without precise vruntime monitoring.

## Fix Summary

The fix restructures the code to ensure `se->exec_start` is available for long sleeper detection during `place_entity()` and is only cleared afterward. It makes three key changes:

**First**, the `se->exec_start = 0` assignment is removed from `migrate_task_rq_fair()`. This preserves `exec_start` through the migration path so that `place_entity()` can correctly compute the actual sleep time.

**Second**, a new `ENQUEUE_MIGRATED` flag is propagated through the enqueue path. In `activate_task()` (core.c), the code now checks `task_on_rq_migrating(p)` and sets `flags |= ENQUEUE_MIGRATED`. In `enqueue_entity()` (fair.c), after `place_entity()` completes, the code resets `exec_start`:
```c
if (flags & ENQUEUE_MIGRATED)
    se->exec_start = 0;
```
This moves the "no longer hot" reset to after placement, preserving the original intent of preventing the task from being considered cache-hot on the new CPU while also allowing `place_entity()` to correctly assess sleep duration.

**Third**, the long sleeper detection is refactored into a new `entity_is_long_sleeper()` helper function with improved logic:
```c
static inline bool entity_is_long_sleeper(struct sched_entity *se) {
    if (se->exec_start == 0)
        return false;
    /* ... compute sleep_time ... */
    if (sleep_time > ((1ULL << 63) / scale_load_down(NICE_0_LOAD)))
        return true;
    return false;
}
```
The function explicitly returns `false` when `exec_start == 0`, providing a safety net even if the ordering is somehow violated. It also handles clock_task divergence between CPUs by returning `false` when `sleep_time <= se->exec_start` (which would indicate the new CPU's clock is behind the old CPU's). The threshold is increased from 60 seconds to approximately 104 days (`2^63 / scale_load_down(NICE_0_LOAD)`), which is the mathematically correct point where vruntime comparison overflow actually becomes possible.

## Triggering Conditions

The bug triggers under the following conditions:

- **Kernel version**: v6.3-rc1 through v6.3-rc3 (commits `829c1651e9c4` present, `a53ce18cacb4` not yet applied). The kernel must have CONFIG_SMP enabled (the migration path is SMP-only).

- **System uptime**: The system must have been running for more than 60 seconds, so that `rq_clock_task()` exceeds `60 * NSEC_PER_SEC`. This condition is met on essentially all real-world systems. In kSTEP, the virtual clock starts at 10 seconds (`INIT_TIME_NS`), so approximately 12,500 default ticks (at ~4ms per tick) or fewer ticks with a larger `tick_interval_ns` are needed to push `rq_clock_task` past 60 seconds.

- **Task migration**: A CFS scheduling entity must undergo migration between CPUs. This happens in three scenarios: (a) A sleeping task wakes up and `select_task_rq_fair()` picks a different CPU (the `TASK_WAKING` path); (b) A running task's CPU affinity changes, causing the scheduler to move it; (c) The load balancer migrates a task during `load_balance()`. All three paths go through `migrate_task_rq_fair()` which triggers the bug.

- **Multi-CPU system**: At least 2 CPUs are required for migration to occur. No specific topology (NUMA, SMT, etc.) is required.

- **Reproducibility**: The bug is **100% deterministic** once the uptime condition is met. Every single CFS task migration triggers the incorrect long sleeper detection. There are no race conditions or timing sensitivities—the ordering of `exec_start` reset before `place_entity()` is architecturally guaranteed by the kernel's migration code path.

- **CFS scheduling class**: Only CFS (SCHED_OTHER/SCHED_BATCH/SCHED_IDLE) entities are affected. RT (SCHED_FIFO/SCHED_RR) and DL (SCHED_DEADLINE) tasks use different migration and placement logic.

## Reproduce Strategy (kSTEP)

The reproduction strategy creates a controlled scenario where a CFS task with accumulated vruntime is migrated between CPUs, and we observe whether its vruntime is correctly preserved (fixed kernel) or incorrectly reset (buggy kernel).

### Step 1: QEMU Configuration

Configure QEMU with at least 2 CPUs. CPU 0 is reserved for the driver, so tasks will run on CPU 1 and CPU 2. No special topology is needed.

### Step 2: Advance Virtual Clock Past 60 Seconds

Since the bug requires `rq_clock_task() > 60 * NSEC_PER_SEC`, and kSTEP starts at 10 seconds, we need to advance the clock. Set `tick_interval_ns = 1000000000ULL` (1 second per tick) in the driver struct, then call `kstep_tick_repeat(55)` at the start to push `rq_clock_task` to ~65 seconds. Alternatively, use the default tick interval and call `kstep_tick_repeat(13000)`, but the large tick approach is faster.

### Step 3: Create Reference Tasks

Create one CFS task (`ref_task`) pinned to CPU 2 using `kstep_task_pin(ref_task, 2, 2)`, wake it, and tick several times so that CPU 2's `cfs_rq->min_vruntime` advances to a known value. This establishes a baseline on the destination CPU.

### Step 4: Create the Migrating Task

Create a CFS task (`mig_task`) pinned to CPU 1 using `kstep_task_pin(mig_task, 1, 1)`. Wake it and let it run for several ticks so it accumulates vruntime significantly above `cpu_rq(1)->cfs.min_vruntime`. Record `mig_task->se.vruntime` as `vruntime_before`.

### Step 5: Block and Migrate

1. Block `mig_task` using `kstep_task_block(mig_task)`. Call `kstep_tick()` to process the block.
2. Change affinity to CPU 2: `kstep_task_pin(mig_task, 2, 2)`.
3. Immediately wake the task: `kstep_task_wakeup(mig_task)`.
4. Call `kstep_tick_repeat(5)` to allow the migration and enqueue to complete.

### Step 6: Observe Vruntime After Migration

Read `mig_task->se.vruntime` as `vruntime_after`. Also read `cpu_rq(2)->cfs.min_vruntime` as `dest_min_vruntime`.

### Step 7: Pass/Fail Criteria

Compute `sleeper_thresh = sysctl_sched_latency >> 1` (assuming GENTLE_FAIR_SLEEPERS is enabled, which is the default).

On the **buggy kernel**:
- `vruntime_after` will be exactly `dest_min_vruntime - sleeper_thresh` because the long sleeper detection fires (due to `exec_start == 0` after migration), and `se->vruntime = vruntime` is used instead of `max_vruntime()`.
- The original accumulated vruntime is completely discarded.

On the **fixed kernel**:
- `vruntime_after` will be `max(adjusted_original_vruntime, dest_min_vruntime - sleeper_thresh)` because `entity_is_long_sleeper()` returns `false` (the task only slept briefly), and the `max_vruntime()` path is correctly taken.
- If the task had accumulated significant vruntime, `vruntime_after` will be higher than `dest_min_vruntime - sleeper_thresh`.

The driver should check whether `vruntime_after > dest_min_vruntime`. If the migrated task had a vruntime ahead of the destination's min_vruntime, it should remain ahead after migration on the fixed kernel. On the buggy kernel, it will be reset to `dest_min_vruntime - sleeper_thresh`, which is behind. Use `kstep_pass()` or `kstep_fail()` accordingly.

### Step 8: Additional Diagnostics

Add logging via `TRACE_INFO` for:
- `mig_task->se.exec_start` before and after migration (on buggy kernel, it will be 0 at place_entity time; on fixed kernel, it will be non-zero at place_entity time and 0 after)
- `mig_task->se.vruntime` at each stage
- `cpu_rq(1)->cfs.min_vruntime` and `cpu_rq(2)->cfs.min_vruntime`
- `rq_clock_task(cpu_rq(2))` to verify it exceeds 60 seconds

### Step 9: Callback Usage

Use `on_tick_end` callback to capture per-tick vruntime snapshots if needed for debugging. The main detection logic can run in the `run()` function after the migration sequence.

### Step 10: Version Guard

Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 1)` or use a version range that includes v6.3-rc1 through v6.3-rc3. Since kSTEP checks out specific commits, the guard should match the kernel version range where `829c1651e9c4` is present but `a53ce18cacb4` is not. In practice, using the rc tags: `>= v6.3-rc1 && < v6.3-rc4`.

### Expected Behavior Summary

| Metric | Buggy Kernel | Fixed Kernel |
|--------|-------------|-------------|
| `exec_start` during `place_entity` | 0 (cleared by `migrate_task_rq_fair`) | Non-zero (preserved from old CPU) |
| Long sleeper detection | Always triggers (sleep_time = rq_clock_task) | Does not trigger (actual sleep time is short) |
| `vruntime` after migration | `min_vruntime - thresh` (reset) | `max(original, min_vruntime - thresh)` (preserved) |
| CFS fairness | Violated (migrated task gets unfair boost) | Maintained |
