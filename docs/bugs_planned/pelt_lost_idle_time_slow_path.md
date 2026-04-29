# PELT: Lost Idle Time Not Tracked on Slow Path

**Commit:** `17e3e88ed0b6318fde0d1c14df1a804711cab1b5`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.18-rc2
**Buggy since:** v5.4-rc1 (introduced by `67692435c411` "sched: Rework pick_next_task() slow-path")

## Bug Description

The PELT (Per-Entity Load Tracking) subsystem maintains a `clock_pelt` on each runqueue that tracks the scaled time reflecting actual computation performed. When a runqueue becomes idle, it must synchronize `clock_pelt` with `rq_clock_task` and, critically, account for any "lost idle time" — periods where the CPU was at maximum utilization and so the PELT clock fell behind real time. This accounting is performed by `update_idle_rq_clock_pelt()`.

The function `pick_next_task_fair()` is called from two distinct paths. The **fast path** (in `__pick_next_task()`) is taken when the previous task is CFS or lower and all runnable tasks are CFS. In this case, `rf` (the `rq_flags` pointer) is non-NULL. The **slow path** is taken when the previous task belongs to a higher-priority scheduling class (RT or DL) or when there are non-CFS tasks on the runqueue. In the slow path, the fair class's `.pick_next_task` method (`__pick_next_task_fair()`) is called, which invokes `pick_next_task_fair(rq, prev, NULL)` — passing `rf = NULL`.

In the buggy code, the `idle:` label in `pick_next_task_fair()` contains an early `return NULL` when `rf` is NULL, which executes before `update_idle_rq_clock_pelt()` is reached. This means that when the slow path is taken and no CFS tasks are available, the PELT lost-idle-time check is entirely skipped. The fix restructures the code so that `update_idle_rq_clock_pelt()` is always called at the `idle:` label, regardless of whether `rf` is NULL.

The scenario that triggers the bug is straightforward: when the last running task on a CPU is an RT or DL task, and that task goes to sleep leaving no CFS tasks behind, the scheduler enters the slow path. The fair class fails to pick a task, reaches the `idle:` label, and returns NULL without accounting for any PELT lost idle time. If the `util_sum` of the runqueue was at its maximum value (indicating the CPU was fully utilized), the accumulated error in `lost_idle_time` grows unboundedly over time.

## Root Cause

The root cause is an incorrect control-flow ordering in `pick_next_task_fair()` at the `idle:` label. In the buggy code:

```c
idle:
    if (!rf)
        return NULL;          /* ← early exit, skips update_idle_rq_clock_pelt */

    new_tasks = sched_balance_newidle(rq, rf);

    if (new_tasks < 0)
        return RETRY_TASK;

    if (new_tasks > 0)
        goto again;

    update_idle_rq_clock_pelt(rq);   /* ← only reached when rf != NULL */

    return NULL;
```

When `rf` is NULL (the slow path), execution hits the `if (!rf) return NULL;` check and returns immediately. The call to `update_idle_rq_clock_pelt(rq)` is never executed.

The function `update_idle_rq_clock_pelt()` performs a critical check: it sums `rq->cfs.avg.util_sum + rq->avg_rt.util_sum + rq->avg_dl.util_sum` and compares it against the divider `((LOAD_AVG_MAX - 1024) << SCHED_CAPACITY_SHIFT) - LOAD_AVG_MAX`. With `LOAD_AVG_MAX = 47742` and `SCHED_CAPACITY_SHIFT = 10`, this evaluates to `(46718 << 10) - 47742 = 47,807,378`. When the aggregate `util_sum` exceeds this threshold, the runqueue has been running at or near full utilization, and the PELT clock (`rq->clock_pelt`) has fallen behind `rq_clock_task(rq)`. The difference `rq_clock_task(rq) - rq->clock_pelt` represents lost idle time that must be accumulated in `rq->lost_idle_time` so that future PELT calculations correctly account for this discrepancy.

The bug was introduced by commit `67692435c411` ("sched: Rework pick_next_task() slow-path") which separated the fast and slow paths. The original intent was that the `sched_balance_newidle()` call (which requires `rf` for rq lock management) should only happen on the fast path. However, the guard `if (!rf) return NULL;` was placed too early, before the `update_idle_rq_clock_pelt()` call which does **not** depend on `rf` and must execute unconditionally.

The slow path is taken whenever `prev->sched_class` is above `fair_sched_class` (i.e., `prev` is an RT, DL, or stop task) OR when `rq->nr_running != rq->cfs.h_nr_queued` (i.e., there exist non-CFS runnable tasks). In `__pick_next_task()`:

```c
if (likely(!sched_class_above(prev->sched_class, &fair_sched_class) &&
           rq->nr_running == rq->cfs.h_nr_queued)) {
    p = pick_next_task_fair(rq, prev, rf);    /* fast path: rf != NULL */
    ...
}
/* else: fall through to slow path */
restart:
    prev_balance(rq, prev, rf);
    for_each_active_class(class) {
        if (class->pick_next_task) {
            p = class->pick_next_task(rq, prev);  /* calls __pick_next_task_fair with rf=NULL */
            ...
```

So whenever a non-CFS task is the previous task and there are no CFS tasks available, the bug is triggered.

## Consequence

The primary consequence is incorrect PELT load tracking. When `update_idle_rq_clock_pelt()` is skipped, `rq->lost_idle_time` does not accumulate the time that should have been tracked. The next time the PELT clock is updated (via `update_rq_clock_pelt()`), the relationship between `clock_pelt` and `rq_clock_task` is incorrect: `clock_pelt` remains behind without the corresponding `lost_idle_time` adjustment.

This leads to inflated utilization signals (`util_avg`, `util_sum`) on the affected runqueue. The PELT signals remain artificially high because the idle period's resynchronization of `clock_pelt` to `rq_clock_task` is done by `_update_idle_rq_clock_pelt()` (which IS called when the idle task starts running), but without the `lost_idle_time` correction, the decay calculation uses an incorrect time base. The effect compounds over repeated cycles of RT/DL task activity followed by idle periods.

Inflated utilization signals cause downstream problems across multiple subsystem consumers. The schedutil cpufreq governor uses `util_avg` to set CPU frequency, so an erroneously high `util_avg` causes the CPU to run at unnecessarily high frequencies, wasting power. Energy-Aware Scheduling (EAS) uses utilization to compute energy costs, so incorrect values lead to suboptimal task placement decisions. The load balancer uses utilization signals to assess CPU load, so an artificially busy-looking CPU receives fewer migrated tasks than it should. On systems with frequent RT/DL workloads (e.g., audio processing pipelines, industrial control systems, or real-time multimedia), this bug causes persistent energy waste and scheduling imbalance.

## Fix Summary

The fix restructures the `idle:` label in `pick_next_task_fair()` so that `update_idle_rq_clock_pelt(rq)` is always called, regardless of the value of `rf`. The `sched_balance_newidle()` call and its associated control flow (`RETRY_TASK` and `goto again`) are wrapped in an `if (rf)` block, while `update_idle_rq_clock_pelt(rq)` is moved outside and after this block:

```c
idle:
    if (rf) {
        new_tasks = sched_balance_newidle(rq, rf);

        if (new_tasks < 0)
            return RETRY_TASK;

        if (new_tasks > 0)
            goto again;
    }

    /* Always called, regardless of rf */
    update_idle_rq_clock_pelt(rq);

    return NULL;
```

This is correct because `sched_balance_newidle()` requires the `rq_flags` parameter to properly release and re-acquire the runqueue lock during idle balancing — it genuinely cannot be called when `rf` is NULL. However, `update_idle_rq_clock_pelt()` has no dependency on `rf` whatsoever; it only reads and writes fields on the `rq` itself (`clock_pelt`, `lost_idle_time`, `cfs.avg.util_sum`, `avg_rt.util_sum`, `avg_dl.util_sum`). Therefore, gating it on `rf` was never intentional — it was an accidental side-effect of the control-flow structure.

The fix is minimal and targeted: only 26 lines change, with 13 insertions and 13 deletions. The logic is identical except for the nesting change at the `idle:` label. No new functions are introduced, and no behavior changes for the fast path (where `rf` is non-NULL, so both `sched_balance_newidle()` and `update_idle_rq_clock_pelt()` are called as before).

## Triggering Conditions

The bug is triggered when all of the following conditions hold simultaneously:

1. **A CPU's last running task is an RT or DL task.** This can be a SCHED_FIFO, SCHED_RR, or SCHED_DEADLINE task. The task must be the only runnable task on that CPU (no CFS tasks queued on the same runqueue).

2. **The RT/DL task goes to sleep or blocks.** This triggers a context switch via `schedule()`, which calls `pick_next_task()`. Since the previous task (`prev`) is RT/DL, `sched_class_above(prev->sched_class, &fair_sched_class)` is true, so the slow path is taken in `__pick_next_task()`.

3. **No CFS tasks are available on the runqueue.** When the slow path iterates through scheduling classes and reaches the fair class, `__pick_next_task_fair()` calls `pick_next_task_fair(rq, prev, NULL)`. Since `pick_task_fair(rq)` returns NULL (no CFS tasks), execution jumps to the `idle:` label.

4. **The runqueue's aggregate PELT util_sum is at or near its maximum.** Specifically, `rq->cfs.avg.util_sum + rq->avg_rt.util_sum + rq->avg_dl.util_sum >= ((LOAD_AVG_MAX - 1024) << SCHED_CAPACITY_SHIFT) - LOAD_AVG_MAX` (approximately 47.8 million). This condition is satisfied when the CPU has been fully utilized (running RT/DL tasks continuously) for long enough that the PELT signals have ramped up to their maximum values. The ramp-up to maximum PELT takes approximately 345ms of continuous full utilization.

5. **The kernel version is between v5.4-rc1 and v6.18-rc1 (inclusive).** The bug was introduced by commit `67692435c411` (merged in v5.4-rc1) and fixed by commit `17e3e88ed0b6318fde0d1c14df1a804711cab1b5` (merged in v6.18-rc2).

No special kernel configuration is required beyond the defaults — `CONFIG_SMP` and `CONFIG_FAIR_GROUP_SCHED` are both enabled in typical kernel configurations, but the bug is present regardless of `CONFIG_FAIR_GROUP_SCHED`. The bug does not involve a race condition; it is a deterministic control-flow error. It triggers every time the slow path is taken with no CFS tasks and the util_sum is at max. The probability of reproduction is 100% if the conditions above are met.

## Reproduce Strategy (kSTEP)

The strategy is to create a scenario where an RT task runs alone on a CPU long enough to saturate the PELT `avg_rt.util_sum` to its maximum, then block the RT task so the CPU becomes idle. On the buggy kernel, `update_idle_rq_clock_pelt()` will be skipped and `lost_idle_time` will not be updated. On the fixed kernel, it will be called and `lost_idle_time` will increase.

### Step-by-Step Plan

1. **Topology and CPU configuration:**
   - Use at least 2 CPUs (`--num_cpus 2`). CPU 0 is reserved for the driver; CPU 1 is the test CPU.
   - No special topology setup is needed (default flat topology is fine).

2. **Task creation and setup:**
   - Create one task via `kstep_task_create()`.
   - Convert it to SCHED_FIFO via `kstep_task_fifo(task)`.
   - Pin it to CPU 1 via `kstep_task_pin(task, 1, 2)` (CPUs 1 through 1).
   - Do NOT create any CFS tasks on CPU 1. This ensures that when the RT task blocks, there are no CFS tasks for `pick_task_fair()` to select, causing `pick_next_task_fair()` to reach the `idle:` label.

3. **Build up PELT utilization:**
   - Wake the RT task: `kstep_task_wakeup(task)`.
   - Tick for a sustained period to ramp up PELT signals. PELT signals converge to steady state in approximately 345ms. With a default tick interval (e.g., 4ms), this requires roughly 100 ticks: `kstep_tick_repeat(100)`.
   - After this, `rq->avg_rt.util_sum` on CPU 1 should be at or near its maximum value (approximately `LOAD_AVG_MAX * SCHED_CAPACITY_SCALE`).

4. **Record pre-block state:**
   - Use `KSYM_IMPORT` to access `cpu_rq` if not already available via `internal.h`.
   - Read and log `rq->lost_idle_time`, `rq->clock_pelt`, `rq_clock_task(rq)`, `rq->avg_rt.util_sum`, `rq->cfs.avg.util_sum`, and `rq->avg_dl.util_sum` from `cpu_rq(1)`.

5. **Trigger the bug:**
   - Block the RT task: `kstep_task_block(task)`. This causes the RT task to sleep, triggering `schedule()` on CPU 1. Since `prev` is an RT task and no CFS tasks exist, the slow path is taken, `__pick_next_task_fair()` is called with `rf = NULL`, `pick_task_fair()` returns NULL, and execution reaches the `idle:` label.
   - On the buggy kernel: `update_idle_rq_clock_pelt(rq)` is NOT called because `!rf` returns NULL early.
   - On the fixed kernel: `update_idle_rq_clock_pelt(rq)` IS called.

6. **Advance time slightly after blocking:**
   - Execute one or a few ticks: `kstep_tick_repeat(2)`. This allows the idle task to run on CPU 1, which will call `update_rq_clock_pelt()` with `is_idle_task(rq->curr)` being true, invoking `_update_idle_rq_clock_pelt()` to sync `clock_pelt` to `rq_clock_task`. However, this does NOT fix the missing `lost_idle_time` — `_update_idle_rq_clock_pelt()` only syncs clocks, it does not check or adjust `lost_idle_time`.

7. **Read and compare post-block state:**
   - Read `rq->lost_idle_time` from `cpu_rq(1)` again.
   - **On the buggy kernel:** `lost_idle_time` will NOT have increased despite the CPU having been at maximum utilization. The gap between `rq_clock_task(rq)` and `clock_pelt` at the moment the RT task blocked represents real lost idle time that was never tracked.
   - **On the fixed kernel:** `lost_idle_time` will have increased by the difference `rq_clock_task(rq) - rq->clock_pelt` that existed when the CPU went idle (because `update_idle_rq_clock_pelt()` was called).

8. **Pass/fail criteria:**
   - Compute the difference in `lost_idle_time` before and after blocking the RT task.
   - If `lost_idle_time` increased (delta > 0): the bug is NOT present → `kstep_pass("lost_idle_time correctly updated: delta=%lu", delta)`.
   - If `lost_idle_time` did NOT increase (delta == 0) despite the aggregate `util_sum` being at max: the bug IS present → `kstep_fail("lost_idle_time not updated on slow path: delta=%lu, util_sum=%u", delta, util_sum)`.

9. **Callbacks:**
   - Use `on_tick_begin` to log the PELT state of CPU 1's runqueue each tick. Output `lost_idle_time`, `clock_pelt`, `avg_rt.util_sum`, and `cfs.avg.util_sum` as JSON for plotting.
   - This allows visual confirmation that `avg_rt.util_sum` ramps up during the RT task's execution and that `lost_idle_time` changes (or doesn't) when the task blocks.

10. **Alternative/additional verification:**
    - After the first block, wake the RT task again, let it run for a few more ticks to re-accumulate utilization, then block it a second time. On the buggy kernel, `lost_idle_time` still won't increase. On the fixed kernel, it will increase each time. This demonstrates the bug compounds over repeated cycles.
    - Optionally, also read `rq->avg_rt.util_avg` and compare it a few hundred milliseconds after the RT task blocks. On the buggy kernel, the util_avg may decay more slowly or incorrectly due to the missing `lost_idle_time` adjustment, though this secondary effect may be subtle.

### Expected Behavior

- **Buggy kernel:** After the RT task blocks, `rq->lost_idle_time` remains at 0 (or whatever its pre-block value was). The `update_idle_rq_clock_pelt()` call is skipped because the `if (!rf) return NULL;` early-exits before reaching it.
- **Fixed kernel:** After the RT task blocks, `rq->lost_idle_time` increases by the gap between `rq_clock_task(rq)` and `rq->clock_pelt`. The `update_idle_rq_clock_pelt()` call executes because the `if (rf)` guard only wraps `sched_balance_newidle()`, not the PELT update.
