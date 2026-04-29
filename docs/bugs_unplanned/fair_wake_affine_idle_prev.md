# Fair: wake_affine Ignores Idle prev_cpu Across Sockets

**Commit:** `d8fcb81f1acf651a0e50eacecca43d0524984f87`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.11-rc1
**Buggy since:** v5.5-rc1 (introduced by commit `11f10e5420f6` "sched/fair: Use load instead of runnable load in wakeup path")

## Bug Description

When a CFS task is woken up, the scheduler must decide whether to run it on the CPU where it last ran (`prev_cpu`) or on the CPU of the waker (`this_cpu`). This decision is made by `wake_affine()`, which first calls `wake_affine_idle()` for a fast idle-based decision, and if that is inconclusive, falls back to `wake_affine_weight()` which compares the effective loads of the two CPUs.

Commit `11f10e5420f6` changed `wake_affine_weight()` to use `cpu_load()` (which returns `cfs_rq_load_avg()`, the PELT load average) instead of `cpu_runnable_load()` (which returns `cfs_rq_runnable_load_avg()`, the runnable load). The crucial difference is that the PELT load average includes decaying contributions from tasks that have already blocked, while the runnable load only accounts for currently runnable tasks. This means an idle CPU that recently ran a short-lived task (such as a kworker from the ondemand cpufreq governor, which fires every 4ms on every core) can appear to have a significant load even though no tasks are actually running on it.

When `prev_cpu` and `this_cpu` are on the same socket (share an LLC), this phantom load on an idle `prev_cpu` is not a problem because `select_idle_sibling()` will later discover that `prev_cpu` is idle and select it anyway. However, when `prev_cpu` and `this_cpu` are on different sockets, `wake_affine_idle()` only returns `prev_cpu` if `this_cpu` is also idle and they share a cache—which they don't across sockets. The function falls through to `wake_affine_weight()`, where the stale PELT load on the idle `prev_cpu` can make it appear more loaded than a busy `this_cpu`, causing the scheduler to incorrectly migrate the waking task to the waker's socket.

In a workload with N mostly-active threads on N cores spread across multiple sockets, these incorrect migrations cascade: a task is pulled to the waker's socket, displacing another task, which then triggers further migrations. The net effect is significant performance degradation on multi-socket NUMA systems, particularly with power management governors like ondemand that generate frequent short-lived kworker activity on every core.

## Root Cause

The root cause lies in the interaction between the PELT load metric and the `wake_affine_idle()` function's incomplete handling of idle CPUs.

Before commit `11f10e5420f6`, `wake_affine_weight()` used `cpu_runnable_load()`, which returned the runnable load average. For an idle CPU with no runnable tasks, this value was effectively zero. So when `prev_cpu` was idle and `this_cpu` was busy (running the waker), the load comparison in `wake_affine_weight()` would naturally favor `prev_cpu` (lower load), causing the function to return `prev_cpu` as the target. The task would stay on its previous socket.

After commit `11f10e5420f6`, `wake_affine_weight()` switched to `cpu_load()`, which returns `cfs_rq_load_avg(&rq->cfs)`. This is the PELT-based load average that decays exponentially with a half-life of ~32ms. Even after all tasks on a CPU have blocked, the load average remains non-zero for a substantial period. A kworker that ran for a fraction of a millisecond can leave a residual load of several hundred (out of 1024 scale) that persists for tens of milliseconds.

The `wake_affine_idle()` function, as it existed before the fix, only handled two cases:
1. If `this_cpu` is idle and shares a cache with `prev_cpu`: prefer `prev_cpu` if it's also idle, otherwise use `this_cpu`.
2. If this is a sync wakeup and `this_cpu` has exactly one running task (the waker): use `this_cpu`.

Critically, there was no case for "prev_cpu is idle but this_cpu is not idle and they don't share a cache." In this situation, `wake_affine_idle()` returned `nr_cpumask_bits` (no decision), causing `wake_affine()` to fall through to `wake_affine_weight()`. There, the stale PELT load on the idle `prev_cpu` was compared against the load on the busy `this_cpu`. The stale load on `prev_cpu` could easily exceed the load on `this_cpu` minus the waker's own contribution, causing `wake_affine_weight()` to select `this_cpu` and migrate the task away from its idle previous CPU to the waker's socket.

The key code path is:
```c
static int wake_affine(struct sched_domain *sd, struct task_struct *p,
                       int this_cpu, int prev_cpu, int sync)
{
    int target = nr_cpumask_bits;

    if (sched_feat(WA_IDLE))
        target = wake_affine_idle(this_cpu, prev_cpu, sync);

    if (sched_feat(WA_WEIGHT) && target == nr_cpumask_bits)
        target = wake_affine_weight(sd, p, this_cpu, prev_cpu, sync);

    if (target == nr_cpumask_bits)
        return prev_cpu;

    return target;
}
```

When `wake_affine_idle()` returns `nr_cpumask_bits` for an idle `prev_cpu` on a different socket, `wake_affine_weight()` makes a decision based on the misleading PELT load average, potentially returning `this_cpu` and forcing a cross-socket migration.

## Consequence

The observable impact is significant performance degradation on multi-socket systems running parallel workloads. Tasks that should remain on their current (idle) CPU are unnecessarily migrated to the waker's socket, breaking cache locality and triggering cascading migrations.

Benchmark results from the commit message show the severity. On an 80-core Intel Xeon E7-8870 v4 system with 160 hardware threads running NAS parallel benchmarks, the `lu.C.x` benchmark with ondemand governor degraded from ~24s (expected, with the fix) to ~36s (buggy), a 50% slowdown. The `splash2x.volrend` benchmark from PARSEC was nearly 2x slower (85 seconds vs 45 seconds). Even with intel_pstate active governor, several benchmarks showed 5-20% slowdowns. The ondemand governor exacerbated the issue because it spawns kworkers every 4ms on every core, maximizing the chance that an idle core has a non-zero PELT load average.

This is not a crash or data corruption bug—it is a performance regression. The scheduler makes suboptimal placement decisions, leading to unnecessary cross-socket migrations, increased cache misses, NUMA traffic, and reduced throughput for parallel workloads. The impact scales with the number of sockets and cores: larger systems with more sockets are affected more severely because there are more opportunities for cross-socket migration decisions.

## Fix Summary

The fix adds a single three-line check to `wake_affine_idle()`, inserted after the existing `sync` check and before the `return nr_cpumask_bits` fallthrough:

```c
if (available_idle_cpu(prev_cpu))
    return prev_cpu;
```

This check catches the case that was previously missed: when `prev_cpu` is genuinely idle (no runnable tasks, verified by `available_idle_cpu()` which checks `rq->nr_running`), `wake_affine_idle()` now immediately returns `prev_cpu` as the target. This short-circuits the fall-through to `wake_affine_weight()`, preventing the stale PELT load average from influencing the decision.

The fix is correct because an idle CPU is always a good target for a waking task—there is zero scheduling latency and the task can benefit from any remaining cache warmth from its previous execution. The fix is also safe because it only triggers when `prev_cpu` is truly idle (no running tasks), so it cannot cause a task to be placed on an overloaded CPU. The existing handling for the cache-shared case (first `if` block) takes priority, so when `this_cpu` is idle and shares a cache with `prev_cpu`, the previous logic still applies. The new check is a superset that handles the cross-socket case where `this_cpu` is busy or not cache-affine.

## Triggering Conditions

The bug requires all of the following conditions to be met simultaneously:

- **Multi-socket (NUMA) topology**: `prev_cpu` and `this_cpu` must be on different sockets (different LLC domains), so that `cpus_share_cache(this_cpu, prev_cpu)` returns false. On a single-socket system, the first `if` block in `wake_affine_idle()` catches the idle `prev_cpu` case.
- **Idle prev_cpu with non-zero PELT load**: `prev_cpu` must be currently idle (`rq->nr_running == 0`) but have a non-zero `cfs_rq_load_avg()` from a recently-blocked task. This happens when a short-lived daemon (e.g., kworker for ondemand governor) ran and blocked within the last ~100ms.
- **Non-idle this_cpu (waker CPU)**: `this_cpu` must not be idle (or if idle, must not share cache with `prev_cpu`), and the wakeup must not be a sync wakeup with exactly one task. This ensures `wake_affine_idle()` falls through to `wake_affine_weight()`.
- **PELT load comparison favoring this_cpu**: The stale PELT load on `prev_cpu` must be high enough relative to the actual load on `this_cpu` that `wake_affine_weight()` returns `this_cpu` instead of failing. This depends on the specific load values and the `sd->imbalance_pct`.
- **WA_IDLE and WA_WEIGHT sched_features enabled**: Both must be enabled (they are by default).

The bug is most reliably triggered with:
- The ondemand cpufreq governor (creates kworkers on every core every 4ms)
- Parallel compute workloads with N threads on N cores across multiple sockets
- 80+ cores across multiple sockets (more cores = more opportunities)

## Reproduce Strategy (kSTEP)

### Why This Bug Cannot Be Reproduced with kSTEP

1. **Kernel version too old**: The fix was merged in v5.11-rc1 and the bug was introduced in v5.5-rc1 (by commit `11f10e5420f6`). kSTEP supports Linux v5.15 and newer only. By v5.15, this fix has already been applied, so there is no buggy kernel version within kSTEP's supported range. Checking out `d8fcb81f1acf~1` would yield a kernel from the v5.10-rc1 era, which is before kSTEP's minimum supported version.

2. **Multi-socket NUMA topology requirement**: Even if the kernel version were supported, reproducing this bug requires a genuine multi-socket topology where `cpus_share_cache()` returns false for CPUs on different sockets. While kSTEP can configure topology via `kstep_topo_*()` APIs, it operates inside QEMU which presents a flat memory model. The `cpus_share_cache()` function relies on the `per_cpu(sd_llc_id, ...)` variable, which kSTEP's topology configuration may be able to set up, but the core issue is that the bug manifests as a performance regression (unnecessary migrations), not a crash or incorrect state that can be point-checked.

3. **PELT load average decay requirement**: The bug depends on PELT load average values being non-zero on an idle CPU after a task blocks. Artificially creating this condition would require running short-lived tasks on specific CPUs and then checking wake_affine decisions within the PELT decay window (~32ms half-life). While kSTEP's tick and sleep primitives could advance time, precisely controlling PELT decay to create the exact load conditions is difficult.

4. **Performance regression, not correctness bug**: The bug manifests as suboptimal task placement decisions (unnecessary cross-socket migrations) leading to performance degradation. It does not cause crashes, hangs, or incorrect kernel state. Detecting it requires measuring migration frequency or task placement patterns across many wakeup events, which is not a simple pass/fail check.

### What Would Need to Change in kSTEP

To support reproducing this class of bug, kSTEP would need:
- Support for kernel versions v5.5 through v5.10 (the affected range).
- The ability to hook into the `wake_affine()` return value or the `select_task_rq_fair()` decision to observe which CPU is selected for a waking task.
- Multi-socket topology with distinct LLC domains where `cpus_share_cache()` properly returns false across sockets.
- A mechanism to create tasks with controlled PELT load history (e.g., run a task for X microseconds then block it, leaving residual PELT load).

### Alternative Reproduction Methods

The original author reproduced the bug by running NAS parallel benchmarks (bt.C, lu.C, sp.C, ua.C) on an 80-core multi-socket Intel Xeon system with 160 hardware threads, using both intel_pstate and ondemand governors. The PARSEC splash2x.volrend benchmark was also effective. The key metrics are wall-clock execution time of these parallel benchmarks: significant (5-50%) slowdowns indicate the bug is active. Using `perf sched` or `trace-cmd` to record scheduling events and count cross-socket migrations would provide more direct evidence.
