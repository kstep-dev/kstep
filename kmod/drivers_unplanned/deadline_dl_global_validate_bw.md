# Deadline: Incorrect Per-CPU vs Per-Root-Domain Bandwidth Comparison in sched_dl_global_validate()

**Commit:** `a57415f5d1e43c3a5c5d412cd85e2792d7ed9b11`
**Affected files:** kernel/sched/deadline.c, kernel/sched/sched.h
**Fixed in:** v5.11-rc1
**Buggy since:** v3.14-rc1 (commit `332ac17ef5bf` — "sched/deadline: Add bandwidth management for SCHED_DEADLINE tasks")

## Bug Description

The function `sched_dl_global_validate()` is called when an administrator changes the global RT bandwidth parameters (`sched_rt_runtime_us` or `sched_rt_period_us`) via sysctl. Its purpose is to validate that the new bandwidth settings are large enough to accommodate all currently allocated SCHED_DEADLINE (DL) bandwidth. If the new per-CPU bandwidth would be smaller than what DL tasks have already reserved, the sysctl write must be rejected with `-EBUSY`.

Under `CONFIG_SMP`, the DL bandwidth accounting structure `dl_bw` is maintained on a per-root-domain basis, not per CPU. A root domain encompasses a set of CPUs that share scheduling state, and the `dl_bw->total_bw` field stores the total DL bandwidth allocated across the entire root domain (i.e., the sum of all DL tasks' bandwidth reservations in that domain).

The buggy code in `sched_dl_global_validate()` computed the new per-CPU bandwidth (`new_bw = to_ratio(period, runtime)`) and compared it directly against `dl_b->total_bw`:

```c
if (new_bw < dl_b->total_bw)
    ret = -EBUSY;
```

This comparison is dimensionally incorrect. `new_bw` represents the bandwidth available on a single CPU, while `dl_b->total_bw` represents the total bandwidth consumed across the entire root domain (which may contain many CPUs). On a system with N CPUs in a root domain, the total available DL bandwidth should be `N * new_bw`, not just `new_bw`. As a result, the validation was far too restrictive — it would reject perfectly valid bandwidth settings on multi-CPU systems, preventing administrators from lowering the RT runtime even when there was ample capacity.

## Root Cause

The root cause is a unit mismatch in the bandwidth comparison inside `sched_dl_global_validate()`. The function computes:

```c
u64 new_bw = to_ratio(period, runtime);
```

This `new_bw` value represents the fraction of a single CPU's time available for DL scheduling. For example, if `sched_rt_runtime_us = 950000` and `sched_rt_period_us = 1000000`, then `new_bw` corresponds to 95% of one CPU.

However, `dl_b->total_bw` is a root-domain-wide aggregate. When a DL task with bandwidth B is admitted to a root domain with N CPUs, B is added to `dl_b->total_bw`. The admission control in `__dl_overflow()` correctly uses the formula:

```c
cap_scale(dl_b->bw, cap) < dl_b->total_bw - old_bw + new_bw
```

where `dl_b->bw` is scaled by the cumulative capacity of the root domain's CPUs. But `sched_dl_global_validate()` did not account for the number of CPUs when comparing against `total_bw`.

The correct comparison should be:

```c
if (new_bw * cpus < dl_b->total_bw)
    ret = -EBUSY;
```

where `cpus` is the number of active CPUs in the root domain, obtained via `dl_bw_cpus(cpu)` (which returns `cpumask_weight_and(rd->span, cpu_active_mask)`).

This bug originated in the initial SCHED_DEADLINE bandwidth management implementation (commit `332ac17ef5bf`), which was merged in v3.14-rc1. At the time, the code had a FIXME comment acknowledging the per-CPU iteration was suboptimal, but the dimensional mismatch in the comparison was not noticed. The misleading comments in `kernel/sched/sched.h` (inherited from SCHED_DEADLINE v2) stated that "the bandwidth is given on a per-CPU basis" and "dl_total_bw array contains, in the i-eth element, the currently allocated bandwidth on the i-eth CPU," which was incorrect for the merged implementation where `dl_bw` is per-root-domain under SMP.

## Consequence

The primary consequence is that `sched_dl_global_validate()` is overly restrictive on SMP systems. Consider a system with 4 CPUs in one root domain, where a single DL task has been admitted with a bandwidth reservation of 25% (0.25 of one CPU). The `dl_b->total_bw` would be approximately `0.25 * (1 << BW_SHIFT)`.

If an administrator attempts to set the RT runtime to 950000 µs (95% of a CPU), `new_bw` would be `0.95 * (1 << BW_SHIFT)`. The correct total available bandwidth is `4 * 0.95 * (1 << BW_SHIFT) = 3.8 * (1 << BW_SHIFT)`, which easily accommodates `0.25 * (1 << BW_SHIFT)`. However, the buggy code compares `0.95` against `0.25`, which would pass in this case but fail in a scenario where multiple DL tasks exist.

More critically, if several DL tasks together consume more than one CPU's worth of bandwidth (e.g., 150% total across 4 CPUs), any attempt to lower the RT bandwidth — even to a value that still accommodates all tasks across the 4 CPUs — would be incorrectly rejected with `-EBUSY`. This prevents legitimate runtime reconfiguration and can cause operational issues in deadline-sensitive deployments where administrators need to tune global bandwidth parameters dynamically.

The bug does not cause crashes, data corruption, or security issues. It is strictly a validation logic error that makes the sysctl interface overly conservative, rejecting valid configurations.

## Fix Summary

The fix adds a single variable `cpus` to `sched_dl_global_validate()`, obtained by calling `dl_bw_cpus(cpu)` for each root domain's representative CPU. The comparison is then changed from:

```c
if (new_bw < dl_b->total_bw)
```

to:

```c
if (new_bw * cpus < dl_b->total_bw)
```

This correctly computes the total available DL bandwidth across all CPUs in the root domain before comparing against the root-domain-wide `total_bw`.

Additionally, the fix updates the misleading comments in `kernel/sched/sched.h` that described `dl_bw` as per-CPU. The old comment stated: "With respect to SMP, the bandwidth is given on a per-CPU basis, meaning that: dl_bw (< 100%) is the bandwidth of the system (group) on each CPU; dl_total_bw array contains, in the i-eth element, the currently allocated bandwidth on the i-eth CPU." The new comment correctly states: "With respect to SMP, bandwidth is given on a per root domain basis, meaning that: bw (< 100%) is the deadline bandwidth of each CPU; total_bw is the currently allocated bandwidth in each root domain."

This fix is correct and complete because it aligns `sched_dl_global_validate()` with the actual per-root-domain semantics of `dl_bw` under CONFIG_SMP, and removes the stale/misleading documentation that had persisted since the SCHED_DEADLINE v2 RFC days.

## Triggering Conditions

To trigger this bug, the following conditions are needed:

- **CONFIG_SMP** must be enabled (on UP systems, `dl_bw` is per-CPU and the comparison is correct).
- A root domain must contain **more than one CPU**. With a single CPU, `cpus = 1` and the buggy and fixed comparisons are equivalent.
- At least one **SCHED_DEADLINE task** must be admitted, contributing some `total_bw` to the root domain's `dl_bw`.
- The total DL bandwidth across all tasks must be **greater than one CPU's worth** of the new bandwidth being set, but **less than or equal to `cpus * new_bw`**. This is the range where the buggy code incorrectly rejects the sysctl write.
- The administrator must attempt to write a new value to **`/proc/sys/kernel/sched_rt_runtime_us`** or **`/proc/sys/kernel/sched_rt_period_us`** via the `sched_rt_handler()` path.

For example, on a 4-CPU system with DL tasks consuming 200% total bandwidth (2 CPUs' worth), attempting to set a per-CPU bandwidth of 60% would fail with the buggy code (`0.60 < 2.00` → `-EBUSY`), but should succeed because the total domain capacity is `4 * 0.60 = 2.40 > 2.00`.

The bug is 100% deterministic — there are no race conditions or timing dependencies. It is triggered every time the above conditions are met.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**

   The fix commit `a57415f5d1e43c3a5c5d412cd85e2792d7ed9b11` was merged during the v5.10-rc1 development cycle and shipped in v5.11-rc1. kSTEP supports Linux v5.15 and newer only. The buggy code (with the incorrect `new_bw < dl_b->total_bw` comparison) does not exist in any kernel version v5.15 or later — it was already fixed in v5.11-rc1. Therefore, kSTEP cannot check out a kernel version that contains the buggy code.

2. **WHAT would need to be added to kSTEP to support this?**

   The only blocker is the kernel version constraint. If kSTEP were extended to support kernels as old as v5.10.x, the bug could potentially be reproduced. The driver would need:
   - A way to create SCHED_DEADLINE tasks (kSTEP currently does not have a `kstep_task_deadline(p, runtime, deadline, period)` API).
   - The ability to write to `sched_rt_runtime_us` and `sched_rt_period_us` sysctls (which kSTEP already supports via `kstep_sysctl_write()`).
   - Multiple CPUs in a single root domain (which kSTEP can configure).
   However, even with these changes, the fundamental v5.15 minimum version requirement prevents reproduction.

3. **The reason is version too old (pre-v5.15).** The fix was applied in v5.11-rc1, and kSTEP's minimum supported version is v5.15. All kSTEP-supported kernels already include this fix.

4. **Alternative reproduction methods outside kSTEP:**

   This bug can be reproduced on any SMP Linux kernel between v3.14 and v5.10.x using the following steps:

   a. Boot a kernel in the affected version range with at least 2 CPUs.
   b. Create one or more SCHED_DEADLINE tasks using `sched_setattr()` with parameters chosen so their total bandwidth exceeds one CPU's worth but fits within the multi-CPU domain. For example, on a 4-CPU system, create 2 DL tasks each with 60% bandwidth (total 120%).
   c. Attempt to write a value to `/proc/sys/kernel/sched_rt_runtime_us` that would provide sufficient per-CPU bandwidth (e.g., 950000 for 95%). The total domain capacity would be `4 * 95% = 380%`, which easily accommodates 120%.
   d. Observe that the write fails with `-EBUSY` when it should succeed.
   e. After applying the fix, the same write succeeds.

   A minimal C program could be written to:
   - Fork DL tasks with `sched_setattr()` setting `sched_runtime`, `sched_deadline`, and `sched_period`.
   - Then attempt `sysctl -w kernel.sched_rt_runtime_us=950000` and check the return value.
   - On buggy kernels with sufficient DL load, this returns `-EBUSY`; on fixed kernels, it succeeds.
