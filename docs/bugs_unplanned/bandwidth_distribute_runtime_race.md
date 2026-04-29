# Bandwidth: Race Between CFS Runtime Distribution and Assignment

**Commit:** `26a8b12747c975b33b4a82d62e4a307e1c07f31b`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.7-rc1
**Buggy since:** v3.17-rc1 (introduced by `c06f04c70489` "sched: Fix potential near-infinite distribute_cfs_runtime() loop")

## Bug Description

The CFS bandwidth controller maintains a global runtime pool (`cfs_b->runtime`) that is refilled each period and distributed to per-CPU `cfs_rq` structures. When the period timer fires and there are throttled cfs_rqs waiting for runtime, `do_sched_cfs_period_timer()` calls `distribute_cfs_runtime()` to hand out runtime to those throttled queues. However, the lock protecting `cfs_b->runtime` must be dropped during distribution because the function needs to acquire per-rq locks (which have a higher lock ordering than `cfs_b->lock`).

The bug is a TOCTOU (time-of-check-time-of-use) race in `distribute_cfs_runtime()`. Before the fix, the function received the current value of `cfs_b->runtime` as a parameter (`remaining`), then operated on this stale local copy while the lock was dropped. During this window, `assign_cfs_rq_runtime()` on other CPUs could concurrently consume `cfs_b->runtime` under the lock. After distribution completed, the caller subtracted the distributed amount from `cfs_b->runtime` via `lsub_positive(&cfs_b->runtime, runtime)`, but since `assign_cfs_rq_runtime()` had already reduced `cfs_b->runtime` independently, the total consumed runtime exceeded the quota.

The problem was amplified by the "slow thread" pattern: tasks that run briefly and then sleep return unused `cfs_rq->runtime_remaining` back to `cfs_b->runtime` via `__return_cfs_rq_runtime()`. If they wake up again quickly, they pull new runtime via `assign_cfs_rq_runtime()`. This churn inflates the amount of runtime flowing through `cfs_b->runtime` during the distribution window, widening the race and causing even more over-allocation. The same race exists in the `do_sched_cfs_slack_timer()` path, which also calls `distribute_cfs_runtime()` with a snapshot and subtracts afterward.

## Root Cause

The root cause lies in the design introduced by commit `c06f04c70489`. That commit changed `distribute_cfs_runtime()` to leave `cfs_b->runtime` accessible to other CPUs during distribution, rather than zeroing it out first. The intent was to prevent a near-infinite loop where cfs_rqs would throttle immediately after unthrottling (because runtime was set to zero), forcing the distribution loop to repeatedly handle the same queues. The trade-off was accepting "limited" over-use of runtime.

In the buggy code, the flow in `do_sched_cfs_period_timer()` was:

```c
while (throttled && cfs_b->runtime > 0 && !cfs_b->distribute_running) {
    runtime = cfs_b->runtime;         // Snapshot: e.g., 10ms
    cfs_b->distribute_running = 1;
    raw_spin_unlock_irqrestore(&cfs_b->lock, flags);

    // distribute_cfs_runtime() distributes from local 'remaining' copy
    // Meanwhile, assign_cfs_rq_runtime() on other CPUs consumes cfs_b->runtime
    runtime = distribute_cfs_runtime(cfs_b, runtime);

    raw_spin_lock_irqsave(&cfs_b->lock, flags);
    cfs_b->distribute_running = 0;
    throttled = !list_empty(&cfs_b->throttled_cfs_rq);
    lsub_positive(&cfs_b->runtime, runtime);  // Subtract distributed amount
}
```

The race proceeds as follows:

1. `do_sched_cfs_period_timer()` refills `cfs_b->runtime` to the quota value (e.g., 10ms) and snapshots `runtime = cfs_b->runtime = 10ms`.
2. It drops `cfs_b->lock` and calls `distribute_cfs_runtime(cfs_b, 10ms)`.
3. `distribute_cfs_runtime()` iterates over throttled cfs_rqs, distributing from its local `remaining` variable. For example, it gives 8ms total to throttled queues.
4. Concurrently, on other CPUs, `update_curr()` → `__account_cfs_rq_runtime()` → `assign_cfs_rq_runtime()` acquires `cfs_b->lock` and subtracts runtime directly from `cfs_b->runtime`. For example, 5ms is consumed, leaving `cfs_b->runtime = 5ms`.
5. `distribute_cfs_runtime()` returns 8ms (the amount distributed from its local copy).
6. The caller does `lsub_positive(&cfs_b->runtime, 8ms)`, which computes `max(0, 5ms - 8ms) = 0`.
7. Total runtime consumed: 8ms (distributed) + 5ms (assigned) = 13ms, but only 10ms was the quota. The 3ms over-allocation is the "leaked" runtime.

The `__return_cfs_rq_runtime()` function makes this worse. When a task dequeues and its `cfs_rq` has no more running tasks, excess `runtime_remaining` (above `min_cfs_rq_runtime` = 1ms) is returned to `cfs_b->runtime`. If tasks wake up quickly and re-acquire runtime via `assign_cfs_rq_runtime()`, `cfs_b->runtime` effectively sees extra "phantom" credits during the distribution window, further inflating the amount that can be double-spent.

The same race also exists in `do_sched_cfs_slack_timer()`, which passes a snapshot of `cfs_b->runtime` to `distribute_cfs_runtime()` and then subtracts the result afterward.

## Consequence

The observable consequence is that a cgroup with CFS bandwidth limits consumes more CPU time than its configured quota permits. The commit message reports a 70% over-use on a 96-core machine: with a 10ms quota in a 100ms period (expected 10% CPU), the "fibtest" workload achieved 17% CPU usage. On a 32-core machine with 96 threads, CPU usage exceeded 12% instead of the expected 10%.

This is a correctness violation of the CFS bandwidth controller. Any workload relying on CPU bandwidth limits for resource isolation — such as containerized environments (Docker, Kubernetes), multi-tenant systems, or latency-sensitive services colocated with batch jobs — can be affected. The over-use scales with the number of CPUs and tasks in the group, as more concurrent `assign_cfs_rq_runtime()` calls widen the race window.

While this does not cause a kernel crash, data corruption, or security vulnerability, it undermines a key isolation mechanism. In production environments where CPU bandwidth limits enforce SLAs or prevent noisy-neighbor effects, this bug can lead to resource contention, degraded performance of neighboring cgroups, and violated capacity planning assumptions.

## Fix Summary

The fix changes `distribute_cfs_runtime()` to acquire `cfs_b->lock` for each per-cfs_rq distribution step, reading and modifying `cfs_b->runtime` atomically rather than operating on a stale snapshot. The function signature changes from `static u64 distribute_cfs_runtime(struct cfs_bandwidth *cfs_b, u64 remaining)` to `static void distribute_cfs_runtime(struct cfs_bandwidth *cfs_b)`.

Inside the distribution loop, for each throttled cfs_rq, the fix acquires `raw_spin_lock(&cfs_b->lock)`, computes the runtime needed (`-cfs_rq->runtime_remaining + 1`), caps it at `cfs_b->runtime`, subtracts it from `cfs_b->runtime`, captures the `remaining` value, and then releases the lock. This ensures that every subtraction from `cfs_b->runtime` is done atomically with the check, eliminating the TOCTOU race with `assign_cfs_rq_runtime()`.

As a result, the callers (`do_sched_cfs_period_timer()` and `do_sched_cfs_slack_timer()`) no longer need the `lsub_positive(&cfs_b->runtime, runtime)` subtraction after distribution, since the distributed amounts are already accounted for inside `distribute_cfs_runtime()`. The return value is removed (now `void`), and the old comment acknowledging the race ("This can result in us over-using our runtime...") is deleted. The `distribute_running` flag is retained to prevent concurrent distribution from the period timer and slack timer.

## Triggering Conditions

The race requires the following conditions:

- **Multiple CPUs**: The race is between `distribute_cfs_runtime()` running on the CPU handling the period timer and `assign_cfs_rq_runtime()` running on other CPUs. More CPUs increase the probability and severity of the race. The original report used 96 cores and 32 cores.
- **CFS bandwidth quota configured**: A cgroup must have `cpu.cfs_quota_us` set to a finite value (i.e., `cfs_b->quota != RUNTIME_INF`). A small quota relative to the number of CPUs increases the race window because more cfs_rqs are throttled and need distribution.
- **Many tasks in the bandwidth-limited cgroup**: Tasks should be spread across multiple CPUs. The more per-CPU cfs_rqs that are active, the more concurrent `assign_cfs_rq_runtime()` calls can occur during distribution.
- **Mixed fast/slow thread pattern**: The race is amplified when some threads sleep and return runtime to `cfs_b->runtime` (via `__return_cfs_rq_runtime()`) while others wake up and pull runtime (via `assign_cfs_rq_runtime()`). This churn through the global pool during the distribution window inflates the over-allocation.
- **Throttled cfs_rqs at period boundary**: The period timer must find throttled cfs_rqs to trigger `distribute_cfs_runtime()`. This happens when the group has exhausted its quota in the previous period.
- **CONFIG_CFS_BANDWIDTH=y**: Must be enabled (it is by default in most distributions).

The race is probabilistic but becomes very likely with many CPUs and tasks. On a 96-core machine, it was observed "routinely" during testing with the "fibtest" workload (1 fast thread computing Fibonacci, 95 slow threads sleeping and waking).

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The fix commit `26a8b12747c975b33b4a82d62e4a307e1c07f31b` was merged into Linux v5.7-rc1. kSTEP only supports Linux v5.15 and newer. Since the buggy code was patched well before v5.15, no kernel version within kSTEP's supported range contains this bug. The buggy `distribute_cfs_runtime()` function with its stale-snapshot-based distribution does not exist in any v5.15+ kernel.

2. **WHAT would need to be added to kSTEP to support this?**
   No changes to kSTEP itself are needed. The only barrier is kernel version support. If kSTEP were extended to support kernels older than v5.15 (specifically v3.17 through v5.6), this bug could be reproduced by:
   - Configuring QEMU with 8+ CPUs
   - Creating a cgroup with `cpu.max` set to a small quota (e.g., "5000 100000" for 5ms/100ms)
   - Creating many tasks (e.g., 16+) spread across CPUs, some sleeping/waking rapidly
   - Observing `cfs_b->runtime` values via `KSYM_IMPORT` to detect over-allocation
   - Comparing total distributed runtime against the refilled quota per period

3. **The reason is version too old (pre-v5.15).**
   The fix is in v5.7-rc1, which predates kSTEP's minimum supported version of v5.15 by approximately 4 major kernel releases (v5.7, v5.8, ..., v5.14, v5.15).

4. **Alternative reproduction methods outside kSTEP:**
   The original developers used a tool called "fibtest" that creates a configurable number of fast and slow threads in a bandwidth-limited cgroup. The reproduction steps are:
   - Create a cgroup with a small CPU quota (e.g., 10ms per 100ms period)
   - Run 1 fast thread (CPU-intensive, e.g., Fibonacci computation) and N-1 slow threads (brief computation then sleep) in the cgroup, where N ≥ number of CPUs
   - Measure actual CPU usage of the cgroup over several seconds
   - On a buggy kernel, CPU usage will significantly exceed the configured quota (e.g., 17% instead of 10%)
   - On a fixed kernel, CPU usage should stay at or near the configured quota
   - This can be done on any real or virtual machine running a kernel between v3.17 and v5.6 with CONFIG_CFS_BANDWIDTH=y
