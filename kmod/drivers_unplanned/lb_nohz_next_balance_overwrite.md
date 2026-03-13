# LB: NOHZ next_balance Overwritten Before Remote CPUs Updated

**Commit:** `3ea2f097b17e13a8280f1f9386c331b326a3dbef`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.9-rc1
**Buggy since:** v4.17-rc1 (introduced by `b7031a02ec75 ("sched/fair: Add NOHZ_STATS_KICK")`)

## Bug Description

In the Linux kernel's NOHZ (tickless) idle load balancing subsystem, the global `nohz.next_balance` timestamp — which controls when the next idle load balance (ILB) will be triggered — is set incorrectly, causing idle CPUs to miss or delay load balancing opportunities. This bug was introduced when commit `b7031a02ec75` split the NOHZ idle balancer into two distinct actions: updating blocked load statistics (`NOHZ_STATS_KICK`) and performing actual load balancing (`NOHZ_BALANCE_KICK`). The split introduced a subtle ordering problem in how `nohz.next_balance` is maintained.

The NOHZ idle load balance mechanism works as follows: when a busy CPU detects that idle CPUs may need rebalancing, it calls `kick_ilb()` to send an IPI to an idle CPU, which then performs load balancing on behalf of all idle CPUs. The `nohz.next_balance` variable tracks when this idle balancing should next occur. In the buggy code, `_nohz_idle_balance()` first processes all remote idle CPUs (updating their stats and optionally running `rebalance_domains()`), then runs `rebalance_domains()` for the local CPU, and only afterward updates `nohz.next_balance` with the earliest `rq->next_balance` seen across all the processed CPUs.

The problem is twofold. First, in `kick_ilb()`, `nohz.next_balance` is unconditionally incremented (`nohz.next_balance++`) even for stats-only kicks (`NOHZ_STATS_KICK` without `NOHZ_BALANCE_KICK`), which advances the next balance time even when no actual rebalancing is requested. Second, in `_nohz_idle_balance()`, the local CPU's `rebalance_domains()` runs before `nohz.next_balance` is updated, so the local CPU's `rebalance_domains()` call — which internally writes to `nohz.next_balance` in a conditional path — can overwrite the value that should reflect the minimum `next_balance` across all remote idle CPUs.

## Root Cause

The root cause lies in two related issues within `kick_ilb()` and `_nohz_idle_balance()` in `kernel/sched/fair.c`.

**Issue 1: Unconditional `nohz.next_balance++` in `kick_ilb()`**

In the buggy code, `kick_ilb()` always increments `nohz.next_balance`:

```c
static void kick_ilb(unsigned int flags)
{
    int ilb_cpu;
    nohz.next_balance++;  // Always incremented, even for NOHZ_STATS_KICK only
    ...
}
```

After the introduction of `NOHZ_STATS_KICK`, `kick_ilb()` can be called with only the stats flag set (no `NOHZ_BALANCE_KICK`). In this case, no actual load balancing will occur, yet `nohz.next_balance` is still advanced. This means the next time `nohz_balancer_kick()` checks `time_after(now, nohz.next_balance)`, it will see a slightly later deadline than expected, potentially delaying the actual ILB kick. Worse, the increment uses `nohz.next_balance++` rather than setting a meaningful jiffies-based time, so if this code path is hit many times in quick succession, the value could drift arbitrarily.

**Issue 2: Wrong ordering of `nohz.next_balance` update in `_nohz_idle_balance()`**

In `_nohz_idle_balance()`, the function iterates over all idle CPUs, runs `rebalance_domains()` for each, and tracks the minimum `rq->next_balance` in a local variable `next_balance`. After the loop, it runs `rebalance_domains()` on the local (this) CPU, and only then writes `nohz.next_balance = next_balance`.

The problem is that `rebalance_domains()` for the local CPU internally executes this code path (in the `#ifdef CONFIG_NO_HZ_COMMON` section at the end of `rebalance_domains()`):

```c
if ((idle == CPU_IDLE) && time_after(nohz.next_balance, rq->next_balance))
    nohz.next_balance = rq->next_balance;
```

This means `rebalance_domains(this_rq, CPU_IDLE)` may update `nohz.next_balance` to `this_rq->next_balance`. But then, the code continues to the `abort:` label where `nohz.next_balance` is overwritten again with the `next_balance` value computed from the remote CPUs loop. This is the correct final value in the non-aborted case, but the ordering creates a window where the global state is inconsistent.

More critically, if `need_resched()` fires during the remote CPU loop, execution jumps to `abort:`, where `nohz.next_balance` is set to `next_balance` — but `next_balance` reflects only the CPUs processed so far, not all idle CPUs. The CPUs that were not yet processed may have earlier `next_balance` deadlines, but those are lost. This means future ILB kicks will be delayed, leaving some CPUs without timely load balancing.

**Issue 3: Update after abort**

In the buggy code, the `nohz.next_balance` update happens after the `abort:` label:

```c
abort:
    if (has_blocked_load)
        WRITE_ONCE(nohz.has_blocked, 1);
    if (likely(update_next_balance))
        nohz.next_balance = next_balance;
    return ret;
```

When the function is aborted due to `need_resched()`, `ret` is false (the full ILB was not completed). In this case, updating `nohz.next_balance` with the partial result is wrong — it should remain at a value that will cause a re-kick soon. The fix moves the `nohz.next_balance` update above the local CPU's `rebalance_domains()` call and before the `abort:` label, so it only happens when the full ILB loop completes successfully.

## Consequence

The observable consequence of this bug is that idle load balancing can be delayed or effectively skipped for extended periods. When `nohz.next_balance` is pushed forward incorrectly — either by the unconditional increment in stats-only kicks, or by an incomplete ILB run updating it with a partial value — the `nohz_balancer_kick()` function on busy CPUs will see that `time_after(now, nohz.next_balance)` is false and skip the ILB kick. This means idle CPUs that could run migrated tasks will remain idle while other CPUs are overloaded.

On systems with many idle CPUs (e.g., servers with power-saving policies, or systems with bursty workloads), this can cause significant load imbalance. Tasks pile up on a few busy CPUs while many CPUs sit idle, leading to reduced throughput, increased latency, and poor CPU utilization. The severity increases with the number of CPUs, because more CPUs means more entries in the `nohz.idle_cpus_mask`, making it more likely that the ILB loop will be interrupted by `need_resched()` before completing.

This bug was reported by Peng Liu, suggesting it was observed in a real-world workload scenario. While it does not cause a crash, kernel panic, or data corruption, it can result in meaningful performance degradation, particularly on systems with heterogeneous or bursty workloads that depend on timely idle load balancing to distribute work efficiently.

## Fix Summary

The fix addresses all three issues with targeted changes:

**Fix 1: Conditional `nohz.next_balance` update in `kick_ilb()`**

The unconditional `nohz.next_balance++` is replaced with a conditional update that only fires for full balance kicks:

```c
if (flags & NOHZ_BALANCE_KICK)
    nohz.next_balance = jiffies + 1;
```

This ensures that stats-only kicks (`NOHZ_STATS_KICK`) do not advance the next balance time. It also replaces the relative increment with an absolute jiffies-based value (`jiffies + 1`), which is more robust — it guarantees the kick will be evaluated on the very next tick, rather than relying on the prior value of `nohz.next_balance` which could be stale.

**Fix 2: Reordered `nohz.next_balance` update in `_nohz_idle_balance()`**

The `nohz.next_balance = next_balance` write is moved from after the `abort:` label to immediately after the remote CPU loop completes (before the local CPU's `rebalance_domains()` and before the `abort:` label). This has two effects:

1. The `nohz.next_balance` is updated with the correct minimum from all remote CPUs before the local CPU's `rebalance_domains()` runs, preventing the local rebalance from clobbering the aggregate value.
2. If the function is aborted due to `need_resched()`, `nohz.next_balance` is not updated at all with partial results. Instead, it retains the value set in `kick_ilb()` (`jiffies + 1`), which ensures the ILB will be re-kicked on the very next tick to process the remaining CPUs.

This fix is correct because: (a) after a complete loop, `next_balance` reflects the true minimum across all idle CPUs; (b) after an incomplete loop, leaving `nohz.next_balance` at `jiffies + 1` guarantees prompt re-kicking; and (c) the local CPU's `rebalance_domains()` can still update `nohz.next_balance` via the `#ifdef CONFIG_NO_HZ_COMMON` path in `rebalance_domains()`, but only to bring it earlier (not later), which is the correct behavior.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

1. **CONFIG_NO_HZ_COMMON must be enabled.** This is the standard configuration for modern Linux kernels (enabled by CONFIG_NO_HZ_IDLE or CONFIG_NO_HZ_FULL).

2. **Multiple CPUs with some idle.** The system must have at least 2 CPUs, with some CPUs in the NOHZ idle state (ticks stopped) and at least one CPU busy enough to trigger the ILB kick. More CPUs increase the probability of hitting the bug, since the ILB loop iterates over `nohz.idle_cpus_mask`.

3. **The ILB kick is triggered via `kick_ilb()`.** This happens when `nohz_balancer_kick()` on a busy CPU detects that idle CPUs need rebalancing and `time_after(now, nohz.next_balance)` is true.

4. **For Issue 1 (stats-only kick):** The `nohz_balancer_kick()` must set only `NOHZ_STATS_KICK` without `NOHZ_BALANCE_KICK`. This occurs when the busy CPU determines that blocked load stats need updating but no actual load migration is needed — e.g., when `nohz.has_blocked` is set and the periodic blocked update timer fires.

5. **For Issue 2 (ordering):** After the ILB CPU starts `_nohz_idle_balance()`, the local CPU's `rebalance_domains()` must produce a `rq->next_balance` that differs from the minimum of the remote CPUs' `next_balance` values. This is common because different CPUs may be on different sched domain levels with different balance intervals.

6. **For Issue 3 (abort path):** The ILB CPU must receive a reschedule request (`need_resched()` returns true) during the `for_each_cpu` loop in `_nohz_idle_balance()`. This can happen if a task is woken on the ILB CPU while it's performing the idle balance. The likelihood increases with more CPUs (longer loop) and higher wakeup rates.

7. **Kernel version between v4.17 and v5.8.** The bug was introduced in v4.17-rc1 by `b7031a02ec75` and fixed before v5.9-rc1.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The bug was fixed in v5.9-rc1 and introduced in v4.17-rc1. kSTEP supports Linux v5.15 and newer only. Since the fix was merged well before v5.15, the bug does not exist in any kernel version that kSTEP can build and test. There is no version of the kernel >= v5.15 that contains the buggy code.

2. **WHAT would need to be added to kSTEP to support this?**
   No kSTEP framework changes are needed. The fundamental limitation is the kernel version support. If kSTEP were extended to support older kernels (v4.17 through v5.8), the bug could potentially be reproduced using kSTEP's existing NOHZ and multi-CPU capabilities. A driver would need to:
   - Configure at least 4 CPUs (to increase the probability of `need_resched()` during the ILB loop).
   - Create tasks that alternate between running and sleeping, causing CPUs to enter and exit NOHZ idle state.
   - Observe `nohz.next_balance` using `KSYM_IMPORT()` to check that it gets pushed forward incorrectly after stats-only kicks or aborted ILB runs.
   - Use `on_sched_softirq_begin/end` callbacks to monitor when the scheduler softirq (which runs `_nohz_idle_balance`) fires and what value `nohz.next_balance` takes.

3. **The reason is version too old (pre-v5.15).** The fix was merged in v5.9-rc1, which is 6 major releases before v5.15. kSTEP cannot build or test kernels in the v4.17–v5.8 range.

4. **Alternative reproduction methods:**
   - Build a v5.7 or v5.8 kernel manually and boot it in QEMU with 4+ CPUs.
   - Write a loadable kernel module that uses `KSYM_IMPORT` (or `kallsyms_lookup_name`) to read `nohz.next_balance` periodically.
   - Create a workload that alternates between high and low CPU usage (e.g., a burst of short-lived tasks followed by idle periods), forcing frequent NOHZ transitions.
   - Instrument `kick_ilb()` and `_nohz_idle_balance()` with `printk()` or tracepoints to observe the value of `nohz.next_balance` at key points: after the `nohz.next_balance++` in `kick_ilb()`, after the remote CPU loop, after the local `rebalance_domains()`, and after the `abort:` label.
   - Compare the observed `nohz.next_balance` progression between a buggy kernel (v5.8) and a fixed kernel (v5.9+) to confirm that stats-only kicks no longer advance it, and that aborted ILB runs no longer overwrite it with partial values.
   - Use `perf sched` or ftrace to observe whether idle CPUs are being woken for load balancing at the expected intervals, or whether they are being left idle longer than necessary.
