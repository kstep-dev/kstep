# EEVDF: Stale Slice Value on Entity Placement

**Commit:** `2f2fc17bab0011430ceb6f2dc1959e7d1f981444`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.6-rc5
**Buggy since:** v6.6-rc1 (introduced by `147f3efaa241` "sched/fair: Implement an EEVDF-like scheduling policy")

## Bug Description

The EEVDF scheduler in Linux uses a per-entity `se->slice` field to represent the time quantum (request length) that a scheduling entity is granted. This slice value is used in `place_entity()` to compute the virtual slice (`vslice = calc_delta_fair(se->slice, se)`) which determines the entity's initial deadline when it is enqueued onto a CFS runqueue. The slice is supposed to track the current global `sysctl_sched_base_slice` value, which can be scaled at boot time based on the number of online CPUs via the `sched_tunablescaling` mechanism.

On SMP systems with the default `SCHED_TUNABLESCALING_LOG` policy, `sysctl_sched_base_slice` is scaled by `1 + ilog2(ncpus)` during boot in `sched_init_granularity()`. For example, on a 4-CPU system the scaling factor is `1 + ilog2(4) = 3`, so the base slice of 750,000 ns (0.75 ms) becomes 2,250,000 ns (2.25 ms). However, the `__sched_fork()` function initializes `se->slice` at fork time from the current value of `sysctl_sched_base_slice`. If a task is forked very early during boot — before `sched_init_granularity()` runs and applies the SMP scaling — its `se->slice` is set to the unscaled (UP) value of 750,000 ns.

The bug is that `place_entity()`, which is called whenever a task is enqueued after sleeping or on initial placement, did not update `se->slice` from the current `sysctl_sched_base_slice`. Only `update_deadline()` performed this update, but `update_deadline()` only fires when a task has consumed its entire current slice (i.e., `se->vruntime >= se->deadline`). Tasks that frequently sleep before exhausting their time slice — such as interactive or I/O-bound tasks — would never trigger `update_deadline()` and therefore retain their stale, unscaled slice value indefinitely.

This means that tasks spawned before the sysctl scaling is applied keep their original (UP) slice length forever, as long as they never run long enough to consume their full slice. These tasks receive artificially shorter deadlines in the EEVDF scheduling order, leading to incorrect scheduling behavior.

## Root Cause

The root cause lies in the asymmetry between the two code paths that set `se->slice`:

1. **`update_deadline()`** (line 977 of `fair.c` pre-fix): Called from `update_curr()` when the entity has consumed its current slice (`se->vruntime >= se->deadline`). This function unconditionally sets `se->slice = sysctl_sched_base_slice` before computing the new deadline. However, it only executes when the task has fully used its current time slice.

2. **`place_entity()`** (line 4920 of `fair.c` pre-fix): Called from `enqueue_entity()` when a task is placed onto the runqueue (e.g., after waking from sleep, or on initial fork placement). This function reads `se->slice` to compute `vslice = calc_delta_fair(se->slice, se)` but does **not** update `se->slice` first. It uses whatever stale value was set at fork time or last `update_deadline()` call.

The problematic code path in the buggy kernel:

```c
static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    u64 vslice = calc_delta_fair(se->slice, se);  // uses stale se->slice!
    u64 vruntime = avg_vruntime(cfs_rq);
    s64 lag = 0;
    // ... lag-based placement logic follows
}
```

The `se->slice` value used here could be the unscaled 750,000 ns from `__sched_fork()` even though `sysctl_sched_base_slice` has since been scaled to a larger value (e.g., 2,250,000 ns on a 4-CPU system). Since `vslice` determines the virtual distance between the entity's `vruntime` and its `deadline`, a smaller stale slice produces a tighter deadline. This means the task appears to have a more urgent deadline than it should, which can affect EEVDF's pick_eevdf() selection.

The condition for the bug is: (a) `sysctl_sched_base_slice` changes after the task's `se->slice` was initialized, AND (b) the task never runs long enough to trigger `update_deadline()`. Condition (a) naturally occurs during boot on any SMP system due to `sched_init_granularity()`, or when an administrator manually writes to the sysctl. Condition (b) is common for interactive, I/O-bound, or frequently-sleeping tasks.

## Consequence

The observable impact is incorrect EEVDF deadline computation for affected tasks. Tasks with a stale, shorter slice value receive tighter deadlines than they should, which causes the EEVDF scheduler's `pick_eevdf()` function to prefer them over tasks with correctly updated (longer) slices. This creates a subtle scheduling fairness imbalance.

On an SMP system with many CPUs, the scaling factor can be significant. For example, on an 8-CPU system, the factor is `1 + ilog2(8) = 4`, meaning the correct slice is 4× the UP value. A task stuck with the UP slice effectively has a deadline that is 4× tighter relative to correctly-placed tasks. This gives it an unfair scheduling advantage: it is more likely to be selected by `pick_eevdf()` as the entity with the earliest eligible deadline, displacing tasks that should have run instead.

While this bug does not cause a crash or hang, it leads to persistent scheduling unfairness for the lifetime of affected tasks. Early-spawned init system tasks (e.g., systemd services, early daemons) that are interactive or I/O-bound would be the primary victims, receiving artificially favorable scheduling treatment. This could also lead to unexpected wakeup latency characteristics, as noted in Peter Zijlstra's cover letter for the patch series, which discusses using slice hints for wakeup latency control — stale slices would undermine any such tuning.

## Fix Summary

The fix adds a single line `se->slice = sysctl_sched_base_slice;` at the beginning of `place_entity()`, before the `vslice` computation:

```c
static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
    u64 vslice, vruntime = avg_vruntime(cfs_rq);
    s64 lag = 0;

    se->slice = sysctl_sched_base_slice;          // NEW: refresh slice
    vslice = calc_delta_fair(se->slice, se);       // now uses current value
    // ... rest unchanged
}
```

This ensures that every time an entity is placed on the runqueue — whether on initial fork placement (`task_fork_fair()` → `place_entity(ENQUEUE_INITIAL)`) or on wakeup (`enqueue_entity()` → `place_entity()`) — its slice is refreshed from the current sysctl value. Together with the existing update in `update_deadline()`, this provides two complementary paths for keeping `se->slice` current: `update_deadline()` handles running tasks that exhaust their slice, and `place_entity()` handles sleeping tasks that re-enter the runqueue.

The fix is correct and complete because `place_entity()` is the gateway for all entities entering the runqueue. The only other significant code path that reads `se->slice` is `update_deadline()`, which already performs its own refresh. Note that the subsequent patch in the same series (patch 2/2, not this commit) later modified this line to be conditional on `!se->custom_slice` to support the `sched_attr::sched_runtime` user-specified slice hint. This fix was a prerequisite for that feature, ensuring that the default path always picks up sysctl changes.

## Triggering Conditions

The bug can be triggered under the following conditions:

- **Kernel version**: v6.6-rc1 through v6.6-rc4 (any kernel with EEVDF, commit `147f3efaa241`, but without the fix `2f2fc17bab00`).
- **SMP system**: The most natural trigger is the boot-time sysctl scaling on any system with 2+ CPUs, where `sched_init_granularity()` multiplies `sysctl_sched_base_slice` by `1 + ilog2(ncpus)`.
- **Task behavior**: The task must never exhaust its full time slice between fork and observation. Specifically, `update_deadline()` must never fire for the task, meaning `se->vruntime` must never reach or exceed `se->deadline`. This is common for tasks that sleep frequently.
- **Sysctl change timing**: The `sysctl_sched_base_slice` value must differ from what was present when the task was forked. This happens naturally at boot (tasks forked before `sched_init_granularity()`) or can be triggered manually by writing to `/proc/sys/kernel/sched_base_slice_ns`.
- **No race conditions**: This is a purely deterministic logic bug, not a race condition. It is 100% reproducible: any task that meets the above criteria will have a stale slice.

The simplest reproduction outside kSTEP: on a multi-CPU system, fork a task, change `sysctl_sched_base_slice`, wake the task, and check `se->slice` before the task runs long enough to trigger `update_deadline()`. On the buggy kernel, `se->slice` retains the old value; on the fixed kernel, it shows the new value.

## Reproduce Strategy (kSTEP)

A kSTEP driver for this bug is straightforward because the conditions are entirely deterministic and require only basic task management and sysctl access. The existing driver at `kmod/drivers/slice_update.c` already demonstrates a working approach. Here is the detailed strategy:

**1. Task creation:**
Create a single CFS task with `kstep_task_create()`. At this point, `__sched_fork()` initializes `task->se.slice = sysctl_sched_base_slice`. The task is not yet on any runqueue (it has not been woken up).

**2. Sysctl modification:**
Use `KSYM_IMPORT(sysctl_sched_base_slice)` to get a pointer to the global variable. Record the original value, then write a distinctly different value (e.g., `original * 10`). This simulates what `sched_init_granularity()` does at boot, or what a manual sysctl write does.

**3. Task wakeup (triggers place_entity):**
Call `kstep_task_wakeup(task)`. This calls `try_to_wake_up()` → `ttwu_do_activate()` → `enqueue_entity()` → `place_entity()`. On the **fixed** kernel, `place_entity()` sets `se->slice = sysctl_sched_base_slice` (the new value) before computing `vslice`. On the **buggy** kernel, it reads the stale `se->slice` (the old fork-time value).

**4. Observation (no ticks):**
Immediately after wakeup (before any ticks fire), read `task->se.slice`. No `kstep_tick()` calls should be made, because ticks would eventually trigger `update_curr()` → `update_deadline()` which would update the slice even on the buggy kernel, masking the bug.

**5. Pass/fail criteria:**
- **Buggy kernel**: `task->se.slice == original_slice` (the fork-time value). `place_entity()` did not update it.
- **Fixed kernel**: `task->se.slice == new_slice` (the modified sysctl value). `place_entity()` refreshed it.

**6. Topology/configuration:**
At least 2 CPUs should be configured (QEMU default is fine). No special topology, cgroup, or priority configuration is needed. The task is a default CFS task at nice 0.

**7. Callbacks:**
No callbacks (`on_tick_begin`, etc.) are needed. The entire test is a linear sequence: create → modify sysctl → wakeup → read → assert.

**8. Expected behavior:**
On the buggy kernel (v6.6-rc1 to v6.6-rc4), the driver should call `kstep_fail()` because `se->slice` remains at the old value. On the fixed kernel (v6.6-rc5+), the driver should call `kstep_pass()` because `se->slice` is updated to the new sysctl value.

**9. Determinism:**
This test is fully deterministic. There are no race conditions, timing dependencies, or probabilistic elements. The bug manifests on every single execution on the buggy kernel.

**10. Kernel version guard:**
The driver should be guarded with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)` since EEVDF was introduced in v6.6-rc1 and is not present in earlier kernels.
