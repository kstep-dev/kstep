# Bandwidth: CFS Period Timer Refills Runtime with Scaled Quota Before Scaling

**Commit:** `5a6d6a6ccb5f48ca8cf7c6d64ff83fd9c7999390`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.8-rc1
**Buggy since:** v5.1-rc5 (commit `2e8e19226398` — "sched/fair: Limit sched_cfs_period_timer() loop to avoid hard lockup")

## Bug Description

The CFS bandwidth controller uses a periodic hrtimer (`sched_cfs_period_timer`) to refill the CPU quota for task groups that have bandwidth limits configured via `cpu.cfs_quota_us` and `cpu.cfs_period_us`. When the timer fires, it calls `hrtimer_forward_now()` to advance the timer to the current time, then calls `do_sched_cfs_period_timer()` to replenish the bandwidth runtime and unthrottle any cfs_rqs that were throttled due to quota exhaustion.

Commit `2e8e19226398` introduced a safeguard against hard lockups in `sched_cfs_period_timer()`. The problem it addressed was that with extremely short `cfs_period_us` values and a large number of child cgroups, `do_sched_cfs_period_timer()` could take longer than the period itself, causing the `for (;;)` loop to never terminate (since `hrtimer_forward_now()` would always return a non-zero overrun count). The safeguard counts loop iterations and, after 3 consecutive iterations, scales up both the period and quota proportionally to give the timer more time to complete its work.

However, the original implementation of this safeguard placed the scaling operation (increasing `cfs_b->period` and `cfs_b->quota`) *before* the call to `do_sched_cfs_period_timer()`. This created a mismatch: the period timer was forwarded using the *old* (smaller) period via `hrtimer_forward_now(timer, cfs_b->period)`, but the subsequent `do_sched_cfs_period_timer()` call refilled the runtime using the *new* (larger, scaled-up) quota. This means tasks receive more runtime than they should for the period that was actually elapsed, violating the configured bandwidth ratio.

## Root Cause

The root cause lies in the ordering of operations within the `for (;;)` loop of `sched_cfs_period_timer()`. In the buggy code, the loop body executes in this order:

1. `hrtimer_forward_now(timer, cfs_b->period)` — advances the timer by the current `cfs_b->period` value. Returns the number of periods that have elapsed (overrun count).
2. If `count > 3`, scale up: `cfs_b->period = ns_to_ktime(new)` where `new = old * 2`, and `cfs_b->quota *= 2`.
3. `idle = do_sched_cfs_period_timer(cfs_b, overrun, flags)` — calls `__refill_cfs_bandwidth_runtime(cfs_b)` which sets `cfs_b->runtime = cfs_b->quota`, then distributes runtime to throttled cfs_rqs.

The critical function `__refill_cfs_bandwidth_runtime()` reads `cfs_b->quota` to determine how much runtime to grant:

```c
void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b)
{
    ...
    cfs_b->runtime = cfs_b->quota;
    ...
}
```

When scaling occurs at step 2, `cfs_b->quota` is doubled. Then at step 3, `do_sched_cfs_period_timer()` refills with this doubled quota. But the timer was only forwarded by the *old* (un-doubled) period at step 1. This means:

- **Before scaling:** period = P, quota = Q → ratio = Q/P
- **After scaling at step 2:** period = 2P, quota = 2Q
- **Timer was forwarded by:** P (the old period, used at step 1)
- **Runtime refilled with:** 2Q (the new quota, used at step 3)

So for that specific iteration, the effective ratio is 2Q/P = 2×(Q/P), which is double the intended bandwidth allocation. Tasks in the cgroup receive twice as much CPU time as their configured quota allows for that period.

On subsequent iterations, `hrtimer_forward_now()` will use the new (larger) period value, so the ratio normalizes. But the first iteration after each scaling event grants excess runtime, which can lead to observable over-scheduling of bandwidth-limited tasks.

Additionally, consider the scenario where the scaling happens on iteration 4 (count reaches 4). The timer was forwarded by old period P on each of the first 4 iterations, but on the 4th iteration, the quota is doubled to 2Q before refilling. The overrun count from `hrtimer_forward_now()` reflected the number of old periods elapsed, but the refill uses the new quota. This mismatch accumulates: if the timer was 4 periods behind, it would refill 4 times, but the 4th refill would use 2Q instead of Q, granting Q extra runtime.

## Consequence

The observable impact of this bug is **incorrect CFS bandwidth enforcement** — tasks in bandwidth-limited cgroups receive more CPU time than their configured quota permits during and immediately after a period/quota scaling event. Specifically:

1. **Over-scheduling:** When the period timer detects it is falling behind (more than 3 overruns) and scales up, the runtime refill on the scaling iteration uses the doubled quota but only covers one old (smaller) period of elapsed time. This grants the cgroup double its intended bandwidth ratio for that period, allowing its tasks to run longer than they should.

2. **Bandwidth guarantee violation:** In container environments (e.g., Kubernetes pods with CPU limits), this means a pod configured with, say, 50% of a CPU could temporarily receive 100% during a scaling event. While this is a transient condition, it violates the isolation guarantees that bandwidth throttling is supposed to provide.

3. **Cascading scaling issues:** Because the runtime is over-refilled, the period timer may not need to throttle tasks that should have been throttled. This can affect the next period's accounting and potentially delay the timer from stabilizing. In the worst case, the timer may continue to accumulate overruns, triggering additional scaling events, each of which grants excess runtime.

The bug is most likely to manifest on systems with very short `cfs_period_us` values and many child cgroups, as these are the conditions that cause `do_sched_cfs_period_timer()` to take long enough to trigger the scaling mechanism. While the excess runtime per event is bounded (at most 2× the quota for one period), in a system that repeatedly triggers scaling, the cumulative effect can be significant.

## Fix Summary

The fix is a simple reordering: `do_sched_cfs_period_timer()` is moved *before* the scaling check, so that runtime is refilled using the current (unscaled) quota before the period and quota are potentially doubled.

In the fixed code, the loop body executes in this corrected order:

1. `hrtimer_forward_now(timer, cfs_b->period)` — advances the timer using the current period.
2. `idle = do_sched_cfs_period_timer(cfs_b, overrun, flags)` — refills runtime with the current `cfs_b->quota` and unthrottles cfs_rqs. This uses the same quota value that corresponds to the period used in step 1.
3. If `count > 3`, scale up: increase both `cfs_b->period` and `cfs_b->quota` proportionally.

This ensures consistency: the period used to advance the timer and the quota used to refill runtime always correspond to the same bandwidth ratio. The scaling only takes effect on the *next* iteration of the loop, where `hrtimer_forward_now()` will use the new (larger) period and `do_sched_cfs_period_timer()` will use the new (larger) quota — maintaining the correct ratio.

The fix is a 4-line change: the line `idle = do_sched_cfs_period_timer(cfs_b, overrun, flags);` is moved from after the scaling block to before it, with a blank line added for readability. The fix was reviewed by Ben Segall and Phil Auld, both of whom confirmed the correctness and noted it was independent of the companion patch in the same series.

## Triggering Conditions

To trigger this bug, the following conditions must all be met:

1. **CFS bandwidth throttling must be enabled:** A task group must have `cpu.cfs_quota_us` set to a finite value (not -1) and `cpu.cfs_period_us` set to a specific period. The system must be using cgroup v1 (`CONFIG_CFS_BANDWIDTH=y`).

2. **Very short period (`cfs_period_us`):** The period must be short enough that `do_sched_cfs_period_timer()` takes longer than the period itself to complete. This is most easily achieved with periods in the range of 1ms–10ms (`1000`–`10000` microseconds).

3. **Many child cgroups or many throttled cfs_rqs:** The `do_sched_cfs_period_timer()` function iterates over `cfs_b->throttled_cfs_rq` list, calling `distribute_cfs_runtime()` and `unthrottle_cfs_rq()` for each entry. `unthrottle_cfs_rq()` calls `walk_tg_tree_from()` which iterates over all child task groups. Having a large number of children (dozens to hundreds) makes this function slow enough to exceed the period.

4. **The period timer must overrun more than 3 times:** The scaling logic only activates when `count > 3`, meaning `hrtimer_forward_now()` must return non-zero for at least 4 consecutive iterations. This happens when the timer fires late (because `do_sched_cfs_period_timer()` took too long) and multiple periods have elapsed since the last successful timer completion.

5. **Kernel version between v5.1-rc5 and v5.8-rc1 (exclusive):** The bug was introduced by commit `2e8e19226398` (merged in v5.1-rc5) and fixed by commit `5a6d6a6ccb5f` (merged in v5.8-rc1). The code path was also backported to some stable kernels, so specific stable kernel versions in the v5.1.x through v5.7.x range are affected.

The bug is deterministic once the scaling condition is triggered — every scaling event will grant excess runtime. However, triggering the scaling condition itself requires specific workload characteristics (many cgroups, short period, heavy CPU usage) that create enough timer overrun.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Kernel Version Too Old

The fix was merged into **v5.8-rc1**, which is before kSTEP's minimum supported kernel version of **v5.15**. The buggy code (with the incorrect ordering) only exists in kernels from v5.1-rc5 through v5.7.x. By the time v5.15 was released, this fix had been in the kernel for over a year. kSTEP cannot check out a kernel old enough to contain this bug.

### 2. What Would Be Needed

Even if kernel version were not an issue, reproducing this bug with kSTEP would require:

- **Creating many cgroups with bandwidth limits:** kSTEP's `kstep_cgroup_create()` and related APIs could create cgroups, but the bug requires a *large* number of child cgroups (dozens to hundreds) to make `do_sched_cfs_period_timer()` slow enough to trigger the scaling mechanism. Creating hundreds of cgroups and populating them with tasks would be tedious but theoretically possible.

- **Setting very short period values:** kSTEP's `kstep_sysctl_write()` could be used to write to `cpu.cfs_period_us`, but the required values (1ms–10ms) are at the lower end of what the kernel allows, and the timing behavior inside QEMU may differ significantly from bare metal.

- **Observing the runtime refill amount:** The bug manifests as `cfs_b->runtime` being set to the *scaled* quota instead of the *original* quota after a scaling event. Detecting this would require reading `cfs_b->runtime` and `cfs_b->quota` at precisely the right moment — after the scaling but before the next refill. kSTEP could potentially use `KSYM_IMPORT` to access these fields, but the timing would be very difficult to control.

- **Generating enough CPU load to trigger throttling and timer overrun:** Multiple tasks would need to consume CPU across many cgroups simultaneously, each getting throttled, to create the conditions where `do_sched_cfs_period_timer()` takes long enough to overrun.

### 3. Alternative Reproduction Methods

Outside of kSTEP, this bug could be reproduced on a kernel in the v5.1–v5.7 range by:

1. Setting up a cgroup hierarchy with 50+ child cgroups, each with a very short `cpu.cfs_period_us` (e.g., 1000–5000 µs).
2. Running CPU-intensive tasks in each child cgroup to trigger bandwidth throttling.
3. Monitoring `cfs_b->runtime` and `cfs_b->quota` via ftrace or a custom kernel module to detect the mismatch when scaling occurs.
4. Observing that tasks in the cgroup receive more CPU time than their configured quota permits during the scaling event, using tools like `perf`, `cgroup stats`, or `/proc/[pid]/schedstat`.

The original reporters did not provide a specific reproduction script; the bug was found through code inspection by Huaixin Chang while working on the burstable CFS bandwidth controller feature.
