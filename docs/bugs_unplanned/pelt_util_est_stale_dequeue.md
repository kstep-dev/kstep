# PELT: Stale CPU util_est Value for Schedutil During Task Dequeue

**Commit:** `8c1f560c1ea3f19e22ba356f62680d9d449c9ec2`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.12-rc1
**Buggy since:** v4.17-rc1 (commit `7f65ea42eb00` "sched/fair: Add util_est on top of PELT")

## Bug Description

When a CFS task is dequeued (e.g., going to sleep), `dequeue_task_fair()` updates PELT load averages and notifies the schedutil cpufreq governor about utilization changes via `cfs_rq_util_change()` → `cpufreq_update_util()`. However, the CPU's root cfs_rq estimated utilization (`rq->cfs.avg.util_est.enqueued`) was not decremented until *after* this notification. This means schedutil would see a stale, inflated CPU utilization estimate that still includes the departing task's contribution.

The CPU utilization used by schedutil is computed as `CPU_utilization = max(CPU_util, CPU_util_est)`, where `CPU_util` is the PELT `util_avg` (which gets properly updated during dequeue via `update_load_avg()`) and `CPU_util_est` is the estimated utilization tracking enqueued tasks. Because `CPU_util_est` was stale (still included the departing task), `CPU_utilization` could be artificially high, causing schedutil to request an unnecessarily high CPU frequency.

This problem manifests during task ramp-down scenarios (a previously busy task going to sleep) and ramp-up scenarios (where the PELT `util_avg` has already decayed but `util_est` remains high from the enqueue). The core issue is an ordering problem: the PELT update and schedutil notification happen before the `util_est` bookkeeping for the departing task, so the cpufreq governor sees an inconsistent snapshot of CPU utilization.

## Root Cause

In the buggy code, `dequeue_task_fair()` performed operations in this order:

1. **First loop** (`for_each_sched_entity`): Calls `dequeue_entity()` which internally calls `update_load_avg()`. This updates `cfs_rq->avg.util_avg` (the PELT signal) and then calls `cfs_rq_util_change()` → `cpufreq_update_util()` → schedutil's `sugov_update_shared/single()` → `sugov_get_util()` → `cpu_util_cfs()`.
2. **Second loop** (`for_each_sched_entity`): Handles entities that were not fully dequeued (parent entities with remaining children), calling `update_load_avg()` again which also triggers `cfs_rq_util_change()`.
3. **After both loops** (at the `dequeue_throttle` label): Calls `util_est_dequeue()` which decrements `cfs_rq->avg.util_est.enqueued` by the departing task's `_task_util_est(p)` and also updates the task's per-entity `util_est` EWMA.

The function `cpu_util_cfs()`, called from within schedutil during step 1 and 2, computes CPU utilization as:
```c
CPU_utilization = max(cpu_util, cpu_util_est)
```
where `cpu_util` is `rq->cfs.avg.util_avg` (already updated to reflect the departing task) and `cpu_util_est` is `rq->cfs.avg.util_est.enqueued` (still including the departing task's contribution). When `cpu_util_est > cpu_util`, the stale `cpu_util_est` dominates and produces an inflated utilization value.

The root cause is that the original `util_est_dequeue()` function combined two logically separate operations: (A) updating the CPU-level `cfs_rq->avg.util_est.enqueued` counter, and (B) updating the task-level `p->se.avg.util_est` EWMA. Operation (A) does not depend on the updated PELT values and can be done before the dequeue loop, but operation (B) depends on the task's updated `util_avg` (from `update_load_avg()` in step 1) and must remain after the loop. Because they were bundled together, both were deferred until after the dequeue loops.

## Consequence

The observable impact is **incorrect CPU frequency selection** during task dequeue. When a high-utilization task goes to sleep, the schedutil cpufreq governor sees a CPU utilization value that is higher than it should be because `util_est` still includes the departing task. This causes the governor to request a higher CPU frequency than necessary.

On mobile/embedded platforms using Energy Aware Scheduling (EAS), this has direct power consumption implications. The CPU runs at an elevated frequency for a period after a busy task sleeps, wasting energy. In ramp-down scenarios (task utilization decreasing), the CPU frequency remains artificially high because `util_est` remembers the peak utilization. In ramp-up scenarios (new high-utilization task starting), the opposite ordering issue can cause a brief moment of incorrect frequency estimation.

While this bug does not cause crashes, hangs, or data corruption, it results in measurable **energy waste** and **suboptimal frequency scaling** on platforms that use schedutil with `util_est` enabled (which is the common configuration on modern ARM platforms). The effect is particularly noticeable on systems with fine-grained DVFS (Dynamic Voltage and Frequency Scaling) where every frequency decision matters for power efficiency.

## Fix Summary

The fix splits the original `util_est_dequeue()` function into two separate functions:

1. **`util_est_dequeue(cfs_rq, p)`** — A new lightweight inline function that handles only the CPU-level `util_est` update: it decrements `cfs_rq->avg.util_est.enqueued` by `_task_util_est(p)` (clamped to avoid underflow). This function is placed **before** the `for_each_sched_entity` dequeue loops in `dequeue_task_fair()`, ensuring that when `update_load_avg()` triggers `cfs_rq_util_change()` and schedutil reads `cpu_util_est`, it already reflects the removal of the departing task.

2. **`util_est_update(cfs_rq, p, task_sleep)`** — The renamed remainder of the original function that handles the task-level `util_est` EWMA update. This function is kept at the original position (after the `dequeue_throttle` label), because it depends on the updated `task_util(p)` value from `update_load_avg()` in the dequeue loops. It handles the EWMA smoothing, the `UTIL_AVG_UNCHANGED` flag check, and the capacity-based skip logic.

The fix is correct because it separates two independent concerns: the CPU aggregate `util_est` counter (which should be decremented immediately to avoid stale values) and the per-task EWMA estimation (which must wait for PELT updates). The `#ifndef CONFIG_SMP` stubs are also updated to provide no-op implementations of both functions. The minor cleanup of removing the `int cpu` local variable (inlining it as `cpu_of(rq_of(cfs_rq))`) is an incidental cleanup.

## Triggering Conditions

To trigger this bug, the following conditions are required:

- **CONFIG_SMP=y** — The util_est code is compiled out on UP kernels.
- **CONFIG_FAIR_GROUP_SCHED** — Not strictly required, but the `for_each_sched_entity` loop structure is relevant; the bug exists even without cgroups as long as `update_load_avg()` triggers `cfs_rq_util_change()`.
- **SCHED_FEAT(UTIL_EST)=1** — The `util_est` feature must be enabled (it is by default).
- **CONFIG_CPU_FREQ=y with schedutil governor active** — The bug's effect is only observable when schedutil is the active cpufreq governor, because that's what reads CPU utilization during dequeue via the `cpufreq_update_util()` callback chain.
- **A CFS task with non-trivial util_est** — The task must have been running long enough to build up a measurable `_task_util_est(p)` value. A task that has completed at least one activation cycle (sleep→wake→sleep) will have `util_est.enqueued` set.
- **The task must be dequeued (going to sleep)** — The stale value is seen specifically during `dequeue_task_fair()`. Any transition that dequeues a CFS task triggers this path.
- **`cpu_util_est > cpu_util` at dequeue time** — The bug only manifests visibly when the stale `util_est` value is the dominant component in `max(cpu_util, cpu_util_est)`. This happens when `util_avg` has already decayed below `util_est`, which is a common scenario because `util_est` tracks the peak (enqueued) utilization while PELT `util_avg` decays continuously.

The bug is highly reliable — it occurs on **every** CFS task dequeue when the above conditions are met. There is no race condition or timing sensitivity; the ordering issue is deterministic in the code structure. The magnitude of the stale value depends on `_task_util_est(p)`, which is the departing task's estimated utilization contribution.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The primary reason is that the **kernel version is too old**. The fix commit `8c1f560c1ea3f19e22ba356f62680d9d449c9ec2` was merged into **v5.12-rc1**, and the buggy kernel (its parent commit) is at the **v5.11-rc2** era. kSTEP supports **Linux v5.15 and newer only**, so the affected kernel versions (v4.17 through v5.11.x) are all outside kSTEP's supported range. The `checkout_linux.py` script cannot build these older kernels with the kSTEP infrastructure.

2. **WHAT would need to be added to kSTEP to support this?**
   No changes to kSTEP's API or framework would help, because the fundamental limitation is kernel version support. If kSTEP were extended to support v5.11 kernels, the bug could potentially be reproduced by:
   - Creating a CFS task pinned to a non-zero CPU and running it for enough ticks to build util_est.
   - Adding kernel-side `printk()` in `cfs_rq_util_change()` to log `cfs_rq->avg.util_est.enqueued` at the moment schedutil is notified.
   - Blocking the task via `kstep_task_block()` to trigger `dequeue_task_fair()`.
   - Parsing the kernel log to verify whether `util_est.enqueued` still includes the departing task's contribution (buggy) or has been decremented (fixed).
   However, even with the correct kernel version, the bug's effect (wrong CPU frequency) would be difficult to observe in QEMU because there is no real cpufreq hardware driver; the schedutil callback chain may not be active without one.

3. **The reason is version too old (pre-v5.15).**
   The fix was merged in v5.12-rc1. kSTEP requires v5.15+. All kernel versions exhibiting this bug (v4.17 through v5.11.x) predate kSTEP's minimum supported version.

4. **Alternative reproduction methods outside kSTEP:**
   - **Real hardware with EAS/schedutil**: Run on an ARM platform (e.g., Hikey960, Dragonboard 845c) with schedutil governor. Create a periodic workload (e.g., `rt-app` with a duty cycle) and trace with `trace-cmd` recording the `sched_util_est_cfs` and `cpu_frequency` tracepoints. Compare the frequency decisions at dequeue time between buggy (v5.11) and fixed (v5.12+) kernels.
   - **ftrace/perf on x86 with cpufreq**: Use an x86 system with `intel_pstate` in passive mode + schedutil. Trace `cpufreq_update_util` and `sched_util_est_cfs_tp` to observe the stale value during dequeue.
   - **Kernel instrumentation**: Add `printk` or `trace_printk` to `cpu_util_cfs()` to log the `util_est` component when called from the dequeue path. This directly reveals the stale value without needing to observe frequency changes.
   - **Synthetic benchmark**: Use a task that alternates between busy (100% CPU for N ms) and sleep periods. Compare the frequency trace during the transition from busy to sleep between buggy and fixed kernels. The buggy kernel will show a brief period where the requested frequency is higher than the fixed kernel.
