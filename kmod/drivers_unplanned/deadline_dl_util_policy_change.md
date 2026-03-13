# Deadline: DL Utilization Tracking Stale During Policy Change

**Commit:** `d7d607096ae6d378b4e92d49946d22739c047d4c`
**Affected files:** kernel/sched/deadline.c
**Fixed in:** v5.14-rc1
**Buggy since:** v4.19-rc1 (commit `3727e0e16340`, "sched/dl: Add dl_rq utilization tracking")

## Bug Description

The SCHED_DEADLINE scheduling class tracks per-runqueue utilization through the `avg_dl` PELT (Per-Entity Load Tracking) structure on each `dl_rq`. This utilization signal is normally updated at three points in the DL scheduling lifecycle: `task_tick_dl()` (during the periodic scheduler tick while a DL task runs), `put_prev_task_dl()` (when a DL task is descheduled), and `set_next_task_dl()` (when a DL task is selected to run next). These three hooks together ensure that `avg_dl` remains a consistent, up-to-date reflection of the DL class's CPU utilization over time.

The bug arises when the currently running task changes its scheduling policy to SCHED_DEADLINE via `sched_setscheduler()`. In this scenario, the task is already executing on the CPU — it is the `rq->curr` task. The policy-change path calls the `switched_to_dl()` callback in `deadline.c` to perform any class-specific setup. However, in the buggy code, `switched_to_dl()` only handles the case where the task is not the current task (`rq->curr != p`): it checks for overloading and preemption. It does nothing when `rq->curr == p`, meaning the `avg_dl` PELT structure's `last_update_time` is never synchronized when a running task transitions into the DL class.

Because `set_next_task_dl()` was never called (the task was already running and was just re-classified, not re-scheduled), the `avg_dl.last_update_time` remains at whatever value it had from the previous DL task's execution on that runqueue — potentially a long time in the past. When the task is eventually descheduled and `put_prev_task_dl()` runs, it calls `update_dl_rq_load_avg()`, which computes a delta from the stale `last_update_time` to the current `rq_clock_pelt()`. This extremely large delta causes a massive spike in the DL utilization signal.

While the PELT signal does self-correct after several milliseconds (through periodic decay in `__update_blocked_others()`), the transient spike has a significant real-world impact because CPU capacity calculations depend on `avg_dl`. An artificially inflated DL utilization reduces the apparent available capacity for CFS tasks, leading to incorrect load balancing decisions, potential task migrations, and suboptimal frequency scaling on systems with schedutil.

## Root Cause

The root cause is a missing `update_dl_rq_load_avg()` call in the `switched_to_dl()` function when the task being switched is the currently running task (`rq->curr == p`).

The PELT framework relies on `last_update_time` to compute time deltas for utilization decay and accumulation. Each scheduling class is responsible for keeping its per-rq PELT structure up-to-date. For the DL class, `update_dl_rq_load_avg(rq_clock_pelt(rq), rq, running)` updates `dl_rq->avg` (aliased as `avg_dl` in the rq) by advancing `last_update_time` to the current clock and decaying/accumulating the utilization signal accordingly.

When a task changes policy to SCHED_DEADLINE while it is currently running, the sequence of events is:

1. `__sched_setscheduler()` is called, which invokes `switched_from_<old_class>()` on the old scheduling class.
2. The task's scheduling class pointer is updated to `dl_sched_class`.
3. `switched_to_dl()` is called on the new class.

In the buggy code, `switched_to_dl()` has this structure:

```c
static void switched_to_dl(struct rq *rq, struct task_struct *p)
{
    /* ... inactive timer handling, !queued handling ... */

    if (rq->curr != p) {
        /* handle preemption / push for non-current tasks */
    }
    /* Nothing happens when rq->curr == p */
}
```

Because `rq->curr == p` (the task is already running), none of the existing code paths execute. Crucially, `update_dl_rq_load_avg()` is never called, so `dl_rq->avg.last_update_time` is not set to the current clock value. It retains whatever stale value it had from the last time a DL task ran on this CPU — which could be seconds or even minutes ago, or zero if no DL task has ever run.

Later, when `put_prev_task_dl()` is called (e.g., when this DL task blocks or is preempted), it invokes `update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 1)`. The PELT computation sees a massive time delta (`rq_clock_pelt(rq) - last_update_time`), which, depending on the PELT decay formula, produces an enormous utilization spike. The PELT decay function `accumulate_sum()` in `kernel/sched/pelt.c` processes this delta in 1ms windows, and a multi-second gap leads to a fully saturated utilization value.

## Consequence

The primary observable consequence is a large transient spike in the `avg_dl` (DL utilization) PELT signal on the affected runqueue. This spike can briefly drive `avg_dl` to its maximum value (1024 in PELT terms), even if the actual DL workload is minimal.

This has cascading effects on scheduler decisions because CPU capacity is computed as `capacity_orig - avg_dl - avg_irq` (simplified). An inflated `avg_dl` reduces the capacity available for CFS tasks, which affects:

- **Load balancing:** CFS load balancing uses `cpu_capacity` (which accounts for `avg_dl`) to determine the fairness of task placement. A transiently reduced capacity can cause the load balancer to pull tasks away from the affected CPU or refuse to accept migrations to it.
- **Frequency scaling:** On systems using the `schedutil` CPUFreq governor, the DL utilization is factored into frequency requests. A spike in `avg_dl` can cause unnecessary frequency increases.
- **Energy-aware scheduling (EAS):** On ARM platforms with EAS, capacity calculations directly influence task placement decisions aimed at energy efficiency. A utilization spike can cause suboptimal placement decisions.

The signal recovers naturally within a few milliseconds as PELT decay brings `avg_dl` back to its true value, but during the transient period, scheduling quality is degraded. On systems with frequent policy changes to SCHED_DEADLINE (e.g., GRUB-like bandwidth servers, or applications using `sched_setattr()` to dynamically enter and exit DL scheduling), the impact can be repeated and significant.

## Fix Summary

The fix adds a two-line `else` branch to the existing `if (rq->curr != p)` check in `switched_to_dl()`:

```c
if (rq->curr != p) {
    /* ... existing preemption/push logic ... */
} else {
    update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 0);
}
```

When the policy-switching task is the currently running task (`rq->curr == p`), the fix calls `update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 0)` with `running=0`. This synchronizes `dl_rq->avg.last_update_time` to the current PELT clock, ensuring that when `put_prev_task_dl()` later calls `update_dl_rq_load_avg()`, the time delta is correct (reflecting only the actual DL execution time, not the entire gap since some previous unrelated DL execution).

The `running=0` parameter is correct here because at the point `switched_to_dl()` is called, the PELT accounting for the task's new class has not yet started. The running contribution will begin being tracked from this point forward, with subsequent calls in `task_tick_dl()` and `put_prev_task_dl()` accumulating the actual DL runtime.

This fix mirrors the companion patch for the RT class (commit `2e4b0fa3f03b`, "sched/rt: Fix RT utilization tracking during policy change") which adds the same pattern to `switched_to_rt()` in `rt.c`. Both patches were submitted together as a v2 series by Vincent Donnefort and reviewed by Vincent Guittot.

## Triggering Conditions

The bug is triggered under these specific conditions:

1. **A task is currently running on a CPU** (`rq->curr == p`).
2. **The task changes its scheduling policy to SCHED_DEADLINE** via `sched_setscheduler()` or `sched_setattr()`. This is the only path that reaches `switched_to_dl()` with `rq->curr == p`.
3. **The `dl_rq->avg.last_update_time` is stale**, meaning either no DL task has recently run on this CPU, or a significant amount of time has passed since the last DL task ran. The larger the gap between `last_update_time` and the current `rq_clock_pelt()`, the larger the utilization spike.
4. **The task is subsequently descheduled** (blocks, yields, or is preempted), causing `put_prev_task_dl()` to be called, which triggers the stale PELT update and produces the spike.

The bug does NOT require:
- Multiple CPUs (it can occur on a single CPU).
- Any specific cgroup configuration.
- Any particular task priority or nice value.
- SMP-specific code paths.

The bug IS more impactful when:
- The DL utilization signal is used for capacity-aware decisions (EAS, schedutil).
- Policy changes happen on CPUs that don't normally run DL tasks (maximizing the staleness of `last_update_time`).
- The task runs for a non-trivial period before being descheduled, as the stale `last_update_time` continues to produce incorrect deltas throughout.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?** The fix was merged in v5.14-rc1 (committed to the `sched/core` tip tree on June 22, 2021, and included in Linus's v5.14-rc1 merge). kSTEP supports Linux v5.15 and newer only. Since v5.15 includes all fixes from v5.14, the buggy code does not exist in any kernel version that kSTEP can build and run. The buggy version of `switched_to_dl()` (without the `else { update_dl_rq_load_avg(...); }` branch) is only present in kernels v4.19-rc1 through v5.13.x.

2. **WHAT would need to be added to kSTEP to support this?** If kSTEP supported kernel versions prior to v5.14, the bug could be reproduced by:
   - Creating a CFS task that is currently running on a CPU.
   - Changing its policy to SCHED_DEADLINE using `sched_setattr()` (kSTEP would need a `kstep_task_deadline(p, runtime, deadline, period)` API to set SCHED_DEADLINE parameters).
   - Waiting for the task to be descheduled.
   - Reading `dl_rq->avg.util_avg` to observe the spike.
   However, the fundamental blocker is kernel version support, not API limitations.

3. **Version too old:** The bug is fixed in v5.14-rc1. kSTEP's minimum supported version is v5.15. The buggy code path does not exist in any kernel kSTEP can run.

4. **Alternative reproduction methods:** To reproduce this bug outside kSTEP, one would need a kernel between v4.19 and v5.13. A simple test program could:
   - Run a CFS task (e.g., a busy loop) on a specific CPU.
   - From another thread or process, call `sched_setattr()` to change the running task to SCHED_DEADLINE with appropriate parameters (runtime, deadline, period).
   - Use `perf` or tracepoints (`sched:pelt_dl_tp`) to observe the `avg_dl` utilization signal on the affected CPU.
   - The signal should show a sharp spike immediately after the policy change, followed by a gradual decay back to the true utilization level.
   - On a patched kernel (v5.14+), no spike should be observed.
