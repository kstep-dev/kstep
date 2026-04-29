# PELT: util_est UTIL_AVG_UNCHANGED Flag Leaks via LSB

**Commit:** `68d7a190682aa4eb02db477328088ebad15acc83`
**Affected files:** kernel/sched/fair.c, kernel/sched/pelt.h, kernel/sched/debug.c, include/linux/sched.h
**Fixed in:** v5.13-rc6
**Buggy since:** v5.13-rc1 (commit b89997aa88f0b "sched/pelt: Fix task util_est update filtering" introduced the `last_enqueued_diff` asymmetry; the `_task_util_est()` leak was present since commit 92a801e5d5b7 "sched/fair: Mask UTIL_AVG_UNCHANGED usages" from v4.20)

## Bug Description

The util_est subsystem maintains a per-task estimated utilization (`util_est`) with two fields: `enqueued` (the task's `util_avg` captured at last dequeue time) and `ewma` (an exponentially weighted moving average). An internal flag `UTIL_AVG_UNCHANGED` is stored inside `util_est.enqueued` to track whether the task's `util_avg` has been updated since enqueue. When the flag is set, `util_est_update()` skips the update at dequeue time, avoiding redundant EWMA recalculations. The flag is cleared by `cfs_se_util_change()` whenever PELT updates occur.

The bug is that `UTIL_AVG_UNCHANGED` was defined as `0x1` (the LSB of `util_est.enqueued`) and was unconditionally OR'd into the return value of `_task_util_est()`. This means `_task_util_est()` always returned a value with bit 0 set, so it could **never return 0**. The function `task_util_est()` returns `max(task_util(p), _task_util_est(p))`, and since `_task_util_est(p)` always returns at least 1, `task_util_est()` can also never return 0 when `SCHED_FEAT(UTIL_EST)` is enabled (the default).

This is a problem because `find_energy_efficient_cpu()` in the Energy Aware Scheduling (EAS) code checks `if (task_util_est(p) == 0)` and returns `prev_cpu` early as an optimization — a task with zero estimated utilization does not need energy-efficient placement. With the bug, this early-return path is dead code: even a newly created task that has never run will appear to have non-zero estimated utilization, causing unnecessary energy calculations on every wakeup of such tasks.

Additionally, commit b89997aa88f0 introduced `last_enqueued_diff` to filter unnecessary `util_est` updates. The computation saved the old `ue.enqueued` into `last_enqueued_diff`, then set `ue.enqueued = (task_util(p) | UTIL_AVG_UNCHANGED)`, and finally computed `last_enqueued_diff -= ue.enqueued`. Because the old `ue.enqueued` might not have the `UTIL_AVG_UNCHANGED` flag (it gets cleared by `cfs_se_util_change()`), but the new value always has it OR'd in, the subtraction produces an off-by-one error. This makes the margin check (`within_margin(last_enqueued_diff, UTIL_EST_MARGIN)`) slightly inaccurate, potentially skipping updates that should proceed or proceeding with updates that should be skipped.

## Root Cause

The root cause lies in the design choice of using the LSB of `util_est.enqueued` to store the `UTIL_AVG_UNCHANGED` flag (defined as `0x1`), combined with exposing this flag through the `_task_util_est()` accessor function.

The function `_task_util_est()` was defined as:
```c
static inline unsigned long _task_util_est(struct task_struct *p)
{
    struct util_est ue = READ_ONCE(p->se.avg.util_est);
    return (max(ue.ewma, ue.enqueued) | UTIL_AVG_UNCHANGED);
}
```

The `| UTIL_AVG_UNCHANGED` (i.e., `| 0x1`) was intended to ensure the flag is preserved when the value is used for enqueue/dequeue accounting on `cfs_rq->avg.util_est.enqueued`. The original intent from commit 92a801e5d5b7 was that losing the LSB for util_est resolution was "certainly acceptable." However, this assumption was wrong for the zero case: when both `ue.ewma` and `ue.enqueued` are 0 (a brand new or barely-used task), the function returns 1 instead of 0.

The second aspect of the bug is in `util_est_update()`. After commit b89997aa88f0, the code does:
```c
last_enqueued_diff = ue.enqueued;  // old value, flag may or may not be set
// ...
ue.enqueued = (task_util(p) | UTIL_AVG_UNCHANGED);  // new value, flag always set
// ...
last_enqueued_diff -= ue.enqueued;  // asymmetric: flag may be in only one operand
```

If the old `ue.enqueued` had the flag cleared (by `cfs_se_util_change()` during a PELT update), then `last_enqueued_diff` does not include the flag bit. But the new `ue.enqueued` always includes it. The difference is thus off by 1 when the flag was cleared in the old value but present in the new value.

The function `cfs_se_util_change()` in `pelt.h` clears `UTIL_AVG_UNCHANGED` whenever `update_load_avg()` is called. Its logic is:
```c
static inline void cfs_se_util_change(struct sched_avg *avg)
{
    enqueued = avg->util_est.enqueued;
    if (!(enqueued & UTIL_AVG_UNCHANGED))
        return;
    enqueued &= ~UTIL_AVG_UNCHANGED;
    WRITE_ONCE(avg->util_est.enqueued, enqueued);
}
```

This correctly clears the flag when util_avg changes, but the asymmetry in `util_est_update()` means the flag's presence/absence contaminates the actual utilization value comparison.

## Consequence

The primary consequence is a performance degradation on systems using Energy Aware Scheduling (EAS), which is the default on mobile/ARM platforms with asymmetric CPU capacities (e.g., big.LITTLE). The `find_energy_efficient_cpu()` function's zero-utilization fast path is completely disabled because `task_util_est()` never returns 0. Every CFS task wakeup — even for tasks with genuinely zero utilization — triggers the full energy computation across all performance domains, wasting CPU cycles on unnecessary calculations.

On symmetric systems without EAS, the impact is more subtle. The off-by-one in `_task_util_est()` causes a minor inflation of the estimated utilization for all CFS tasks. This feeds into `uclamp_task_util()`, misfit task detection (`update_misfit_status()`), and capacity checks (`task_fits_capacity()`), potentially causing slightly suboptimal scheduling decisions. The `last_enqueued_diff` asymmetry can cause the within-margin filter to make incorrect decisions about whether to update `util_est`, leading to stale or unnecessarily updated EWMA values.

There is no crash, hang, or data corruption. The impact is strictly a performance/efficiency issue: wasted energy calculations on EAS systems, and slightly inaccurate util_est values affecting scheduling placement heuristics on all systems. The debug output via `proc_sched_show_task()` also exposes the raw `UTIL_AVG_UNCHANGED` flag in `util_est.enqueued`, showing inflated values to administrators inspecting `/proc/<pid>/sched`.

## Fix Summary

The fix changes the `UTIL_AVG_UNCHANGED` flag from the LSB (`0x1`) to the MSB (`0x80000000`) of `util_est.enqueued`. Since the maximum possible `util_avg` for a task is 1024 (`SCHED_CAPACITY_SCALE`), the MSB of an `unsigned int` is never used for actual utilization values, making it safe to repurpose.

The key changes are:

1. **`_task_util_est()`** now strips the flag before returning: `return max(ue.ewma, (ue.enqueued & ~UTIL_AVG_UNCHANGED))`. This ensures the flag is never leaked to callers, and the function correctly returns 0 when both `ewma` and `enqueued` are 0.

2. **`util_est_update()`** no longer mixes the flag into the computation. The new enqueued value is first computed cleanly as `ue.enqueued = task_util(p)` (without the flag). The flag is only OR'd in at the very end, after all arithmetic, at the `done:` label: `ue.enqueued |= UTIL_AVG_UNCHANGED`. This eliminates the off-by-one asymmetry in `last_enqueued_diff`.

3. **`UTIL_AVG_UNCHANGED` definition** is moved from `kernel/sched/pelt.h` to `include/linux/sched.h` (inside `struct util_est`), changed from `0x1` to `0x80000000`, and the comment is updated to explain the MSB usage.

4. **`proc_sched_show_task()`** in `debug.c` now masks the flag when displaying `util_est.enqueued`: `PM(se.avg.util_est.enqueued, ~UTIL_AVG_UNCHANGED)`, ensuring debug output shows the actual utilization value.

The `cfs_se_util_change()` logic in `pelt.h` is unchanged in structure — it still clears the flag when util_avg is updated — but the comment is corrected from "Avoid store if the flag has been already set" to "Avoid store if the flag has been already reset," reflecting that the flag is now cleared (reset) to indicate a util_avg change occurred.

## Triggering Conditions

To trigger the `_task_util_est()` never-returning-0 bug:
- `CONFIG_SMP` must be enabled (util_est is SMP-only).
- `SCHED_FEAT(UTIL_EST)` must be true (the default).
- A CFS task must exist with both `util_est.ewma == 0` and `util_est.enqueued == 0` (or with `UTIL_AVG_UNCHANGED` cleared). This is the case for a newly forked task that has not yet completed a full activation cycle, or a task whose utilization has decayed to zero.
- On an EAS-enabled system (asymmetric CPU capacity with an energy model registered), the task is woken up and `find_energy_efficient_cpu()` checks `task_util_est(p) == 0`. With the bug, this check always fails, so the early-return is never taken.

To trigger the `last_enqueued_diff` asymmetry:
- A CFS task must have had its `util_avg` updated at least once between enqueue and dequeue (so `cfs_se_util_change()` clears the `UTIL_AVG_UNCHANGED` flag from `ue.enqueued`).
- The task must then dequeue via `task_sleep == true`, entering `util_est_update()`.
- The old `ue.enqueued` will have the flag cleared, but `ue.enqueued = (task_util(p) | UTIL_AVG_UNCHANGED)` always sets it, causing an off-by-one in `last_enqueued_diff`.

The bug is 100% deterministic — it occurs every time util_est is active (the default). Every CFS task wakeup on an EAS system hits the dead `task_util_est() == 0` check. The `last_enqueued_diff` off-by-one occurs every time a task sleeps after having had at least one PELT update while enqueued.

The bug requires no specific topology, no specific CPU count, no cgroup configuration, and no race conditions. It is a pure logic error in how an internal flag bit is handled, and it triggers unconditionally under normal CFS scheduling.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP because the kernel version is too old.

1. **WHY can this bug not be reproduced with kSTEP?** The fix commit `68d7a190682a` was merged into `v5.13-rc6`. kSTEP supports Linux v5.15 and newer only. The buggy kernels (v5.13-rc1 through v5.13-rc5, or more broadly any kernel with commit 92a801e5d5b7 but without 68d7a190682a) are all from the v5.13 development cycle, which is before the v5.15 minimum required by kSTEP. Building and running a v5.13-rc4/rc5 kernel in the kSTEP QEMU environment is not supported.

2. **WHAT would need to be added to kSTEP to support this?** No new kSTEP APIs or framework changes are needed. The only barrier is the kernel version constraint. If kSTEP's supported kernel range were extended back to v5.13, the bug could be reproduced straightforwardly by:
   - Creating a CFS task with `kstep_task_create()`
   - Reading `p->se.avg.util_est.enqueued` and `p->se.avg.util_est.ewma` (both should be 0 for a new task)
   - Computing `_task_util_est(p)` equivalent: `max(ewma, enqueued) | 0x1` = `0 | 0x1` = 1 (buggy) vs. `max(ewma, enqueued & ~0x80000000)` = 0 (fixed)
   - Verifying with `kstep_pass`/`kstep_fail` that the function returns 0 on fixed and non-zero on buggy

3. **The reason is version too old (pre-v5.15).** The fix was merged into v5.13-rc6. All kernels from v5.13 onward (including v5.15) already contain this fix. The buggy window (v5.13-rc1 to v5.13-rc5) is entirely before v5.15.

4. **Alternative reproduction methods outside kSTEP:**
   - **Tracing on a v5.13-rc4 kernel:** Boot a v5.13-rc4/rc5 kernel on real or virtual hardware. Use `trace-cmd` or `perf` to trace the `sched_util_est_se` tracepoint. Create a task that sleeps immediately. Observe that `util_est.enqueued` reported in the trace always has the LSB set, even when the actual utilization is 0.
   - **Code inspection / static analysis:** The bug is deterministic and can be verified by reading `_task_util_est()`: the `| UTIL_AVG_UNCHANGED` with `UTIL_AVG_UNCHANGED = 0x1` guarantees the return value is always ≥ 1.
   - **Unit testing in a VM:** Boot a v5.13-rc4 kernel in QEMU, load a kernel module that reads `task_util_est()` for a newly created kthread, and verify it returns 1 instead of the expected 0.
   - **EAS energy calculation counting:** On an ARM big.LITTLE system running v5.13-rc4 with EAS enabled, count how many times `find_energy_efficient_cpu()` performs full energy computation vs. taking the early-return path. With the bug, the early-return count should always be 0. After the fix, newly created or idle tasks should hit the early return.
