# PELT: RT utilization tracking spike during policy change

**Commit:** `fecfcbc288e9f4923f40fd23ca78a6acdc7fdf6c`
**Affected files:** kernel/sched/rt.c
**Fixed in:** v5.14-rc1
**Buggy since:** v4.19-rc1 (commit `371bf4273269` "sched/rt: Add rt_rq utilization tracking")

## Bug Description

The Linux kernel's PELT (Per-Entity Load Tracking) framework tracks per-runqueue utilization for RT tasks through the `avg_rt` structure on each `struct rq`. This utilization signal is a critical input to the scheduler's CPU capacity estimation — the kernel subtracts `avg_rt.util_avg` from a CPU's total capacity to determine how much capacity is available for CFS tasks and for Energy Aware Scheduling (EAS) decisions.

The `avg_rt` structure is updated during three key RT scheduling operations: `task_tick_rt()` (every scheduler tick while an RT task runs), `put_prev_task_rt()` (when an RT task is descheduled), and `set_next_task_rt()` (when an RT task is selected to run). The `set_next_task_rt()` function specifically handles the transition from non-RT to RT execution: it calls `update_rt_rq_load_avg()` only when the previous task's class was not RT, thereby initializing the tracking window for the new RT execution period.

However, there is a gap in this tracking when a currently running CFS task changes its scheduling policy to `SCHED_FIFO` or `SCHED_RR` (RT policies) via `sched_setscheduler()`. In this scenario, the task is already running on the CPU when the class change occurs. The kernel calls `switched_to_rt()` for the task, but the original code simply returned early for a currently-running task (since `rq->curr == p` meant the `task_on_rq_queued(p) && rq->curr != p` condition was false). No `update_rt_rq_load_avg()` call was made, leaving the `avg_rt.last_update_time` stale from whenever it was last updated (potentially a long time ago if no RT tasks had recently run on this CPU).

When the task is eventually descheduled (via `put_prev_task_rt()`), `update_rt_rq_load_avg()` is called with the current PELT clock but with a `last_update_time` that is far in the past. The `___update_load_sum()` function then computes an enormous delta, causing a massive spike in the `avg_rt.util_avg` signal. This spike artificially inflates the RT utilization, reducing the perceived available CPU capacity and disrupting CFS scheduling, load balancing, and EAS decisions.

## Root Cause

The root cause lies in the `switched_to_rt()` function in `kernel/sched/rt.c`. Before the fix, this function contained the following logic:

```c
static void switched_to_rt(struct rq *rq, struct task_struct *p)
{
    if (task_on_rq_queued(p) && rq->curr != p) {
        /* push/preempt logic for non-running queued tasks */
    }
}
```

The condition `rq->curr != p` meant that when the task being switched to RT was the currently running task (`rq->curr == p`), the entire function body was skipped. The implicit assumption was "if we are already running, there's nothing to do." This assumption was correct for scheduling priority and push/pull operations, but it completely missed the PELT utilization tracking requirement.

The PELT tracking for `avg_rt` works by maintaining a `last_update_time` in the `struct sched_avg` embedded in `rq->avg_rt`. Each call to `update_rt_rq_load_avg()` computes the time delta since `last_update_time`, accumulates utilization for the period, and updates `last_update_time` to the current clock. The `running` parameter (0 or 1) indicates whether RT tasks are currently executing on the runqueue.

In `set_next_task_rt()`, the load average is updated with `running=0` specifically when transitioning from a non-RT previous task:

```c
if (rq->donor->sched_class != &rt_sched_class)
    update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);
```

This call marks the boundary: "up until now, no RT was running." But when a policy change occurs on the currently running task, `set_next_task_rt()` is called via `check_class_changed()` → `set_next_task()`, yet the `first` parameter is `false` in this path (since we're not doing a full pick-next), so the `update_rt_rq_load_avg()` call inside `set_next_task_rt()` is skipped due to the early return:

```c
if (!first)
    return;
```

This means neither `set_next_task_rt()` nor `switched_to_rt()` updates `avg_rt` when a running task switches policy to RT. The `last_update_time` remains at whatever value it had from the last RT activity on this CPU, which could be milliseconds or even seconds in the past.

Later, when `put_prev_task_rt()` runs (e.g., when this task blocks or gets preempted), it calls `update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 1)` with `running=1`. The PELT machinery sees a huge time delta and computes the utilization as if RT tasks had been running for that entire gap. This produces a spike in `avg_rt.util_avg` that can approach the CPU's maximum scale value (1024).

## Consequence

The immediate consequence is a transient but large spike in the `avg_rt.util_avg` signal for the affected CPU. Since `avg_rt.util_avg` is subtracted from the CPU's total capacity to determine available capacity for CFS tasks, a spike causes the scheduler to believe the CPU has much less capacity available than it actually does.

This has several downstream effects:
1. **EAS (Energy Aware Scheduling) misplacement**: The energy model uses CPU capacity to estimate energy cost. Inflated RT utilization makes the CPU appear overutilized, causing EAS to place tasks on other CPUs (potentially less energy-efficient ones). On heterogeneous (big.LITTLE) systems, this is particularly impactful as it can push CFS tasks to big cores unnecessarily, increasing power consumption.
2. **Load balancing disruption**: The CFS load balancer uses available capacity to determine group imbalance. A capacity reduction due to the spike can trigger unnecessary migrations or prevent beneficial ones. The CPU may be classified as overutilized even when it has ample real capacity.
3. **CPU frequency scaling**: On systems using `schedutil`, the inflated utilization can cause the CPU frequency governor to request higher frequencies than needed, wasting energy.

The signal eventually recovers because `__update_blocked_others()` periodically updates `avg_rt` even when no RT tasks are running, causing the inflated value to decay. However, the recovery takes multiple PELT half-life periods (approximately 16ms each), meaning the disruption can last tens of milliseconds — long enough to cause measurable scheduling quality degradation, especially on latency-sensitive or energy-constrained workloads like those on mobile devices.

## Fix Summary

The fix modifies `switched_to_rt()` to explicitly handle the case where the currently running task switches to RT policy. Instead of skipping the function entirely for `rq->curr == p`, the fix adds a dedicated code path:

```c
if (task_current(rq, p)) {
    update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0);
    return;
}
```

The call to `update_rt_rq_load_avg(rq_clock_pelt(rq), rq, 0)` with `running=0` synchronizes the `avg_rt.last_update_time` to the current PELT clock without accumulating any RT running time. This is semantically correct: up to this point, no RT tasks were running on this CPU (the task was CFS), so the update should reflect zero RT utilization for the elapsed period. After this call, `last_update_time` is current, so when `put_prev_task_rt()` later calls `update_rt_rq_load_avg()` with `running=1`, the delta will correctly reflect only the actual RT execution time.

The fix also simplifies the non-running path. The old condition `task_on_rq_queued(p) && rq->curr != p` is replaced with just `task_on_rq_queued(p)`, since the `rq->curr == p` case is already handled by the early return above. This makes the code cleaner and ensures all cases are explicitly covered: running tasks get a load update, queued non-running tasks get push/preempt handling, and non-queued tasks (not possible in this context, but defensively handled) fall through.

## Triggering Conditions

The bug is triggered by the following precise sequence:

1. **A CFS task is currently running on a CPU.** The task must be the `rq->curr` on that runqueue.
2. **No RT tasks have recently run on the same CPU.** This ensures `avg_rt.last_update_time` is stale (far in the past relative to the current PELT clock). The longer the gap since the last RT activity, the larger the spike.
3. **The running CFS task's scheduling policy is changed to an RT policy** (`SCHED_FIFO` or `SCHED_RR`) via `sched_setscheduler()` or `sched_setattr()`. This triggers the `switched_to_rt()` callback.
4. **The task continues running as RT for some time**, then is descheduled (blocks, yields, or is preempted). The `put_prev_task_rt()` call at descheduling time triggers the utilization spike.

The bug does not require any special kernel configuration beyond `CONFIG_SMP` (for PELT tracking to be active and `avg_rt` to be used in capacity calculations). It does not depend on the number of CPUs, though the impact is more visible on systems with capacity-aware scheduling (heterogeneous architectures) or EAS enabled.

The bug is deterministic and 100% reproducible: every policy change from CFS to RT on a running task where `avg_rt.last_update_time` is stale will produce the spike. The magnitude of the spike is proportional to the time gap between the last RT activity and the policy change.

There are no race conditions involved — the bug occurs in a single-threaded code path under the runqueue lock. The `sched_setscheduler()` syscall holds `rq->lock` throughout the class change, so the sequence of `check_class_changed()` → `switched_to_rt()` is atomic with respect to other scheduling operations.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The fix was merged into the `sched/core` tip branch and included in Linux v5.14-rc1 (the commit's `git describe --contains` resolves to v5.14-rc1). The parent commit (the buggy version, `fecfcbc288e9~1`) is at v5.13-rc6. kSTEP supports Linux v5.15 and newer only. Since the bug was already fixed before v5.15, checking out the buggy commit (`fecfcbc288e9~1`) produces a v5.13-rc6 kernel that cannot be built or run with kSTEP. The bug does not exist in any kernel version that kSTEP supports.

2. **WHAT would need to be added to kSTEP to support this?**
   kSTEP would need to support Linux kernel versions v5.13 or earlier (specifically any version between v4.19-rc1 and v5.14-rc1 exclusive). This is a fundamental version compatibility requirement, not a feature addition. The kSTEP build system, module API, and internal header dependencies are all designed for v5.15+. Supporting v5.13 would require backporting kSTEP's module interface to match the older kernel's internal scheduler structures, which differ significantly (e.g., no `rq->donor`, different `set_next_task` signature, different PELT internals).

3. **The reason is kernel version too old.** The fix targets v5.14-rc1 (committed June 22, 2021; merged before v5.14-rc1 tag). The buggy kernel at `fecfcbc288e9~1` is v5.13-rc6. kSTEP's minimum supported version is v5.15. The bug was already fixed approximately 4 months before v5.15 was released.

4. **Alternative reproduction methods outside kSTEP:**
   This bug can be reproduced on a real or virtual machine running a kernel between v4.19 and v5.13 (inclusive). The reproduction steps are straightforward:
   - Boot a v5.13 kernel (or any kernel in the affected range) with `CONFIG_SMP=y` and `CONFIG_SMP_PELT=y` (default in most configs).
   - Run a CFS task on a CPU where no RT tasks have run recently.
   - Use `sched_setscheduler()` to change the task's policy to `SCHED_FIFO`.
   - Read `/proc/schedstat` or use ftrace with the `sched:pelt_rt` tracepoint to observe `avg_rt.util_avg` before and after the policy change.
   - The spike will be visible as an anomalously high `util_avg` value (potentially close to 1024) immediately after the task is descheduled from RT.
   - Compare with a v5.14+ kernel where the spike does not occur.

   Alternatively, one could add a `printk()` in `update_rt_rq_load_avg()` to log the delta and the resulting `util_avg`, or use BPF tracing on a live system to capture the PELT update events.
