# Uclamp: Fast Path Performance Regression from Unconditional Uclamp Logic

**Commit:** `46609ce227039fd192e0ecc7d940bed587fd2c78`
**Affected files:** kernel/sched/core.c, kernel/sched/sched.h, kernel/sched/cpufreq_schedutil.c
**Fixed in:** v5.9-rc1
**Buggy since:** v5.3-rc1 (commit 69842cba9ace "sched/uclamp: Add CPU's clamp buckets refcounting")

## Bug Description

When `CONFIG_UCLAMP_TASK` is enabled in the kernel configuration, the utilization clamping (uclamp) logic executes unconditionally on every `enqueue_task()` and `dequeue_task()` call in the scheduler's fast path, even when no userspace consumer has ever requested any utilization clamping. This causes a measurable performance regression compared to kernels compiled without `CONFIG_UCLAMP_TASK`.

The regression was reported by Mel Gorman using netperf UDP_STREAM benchmarks on a 2-socket Xeon E5 2x10-core system. The uclamp-enabled kernel showed a consistent 1.5–3% throughput regression across all packet sizes (64 bytes to 16384 bytes) compared to a kernel compiled without uclamp support. For example, send-1024 throughput dropped from 2525.28 to 2448.26 (−3.05%), and send-16384 from 26386.47 to 25752.25 (−2.40%).

Investigation by Qais Yousef found no single expensive operation in the uclamp code, but profiling showed increased overhead in `try_to_wake_up()`, `activate_task()`, and `deactivate_task()` — all of which call into `uclamp_rq_inc()` and `uclamp_rq_dec()`. The overhead was attributed to poor instruction/data cache behavior caused by touching uclamp data structures (per-CPU `struct uclamp_rq`, per-task `struct uclamp_se`) on every enqueue/dequeue, even when those structures held only default values and no actual clamping was in effect.

The bug was particularly problematic for Linux distribution kernels, which want to compile in uclamp support by default (for users who need it) without penalizing the vast majority of workloads that never use uclamp.

## Root Cause

The root cause is that commit 69842cba9ace ("sched/uclamp: Add CPU's clamp buckets refcounting") introduced per-CPU `struct uclamp_rq` structures with bucket-based refcounting that is updated on every task enqueue and dequeue. The functions `uclamp_rq_inc()` and `uclamp_rq_dec()` are called from `enqueue_task()` and `dequeue_task()` respectively, which are the hottest paths in the scheduler.

In `uclamp_rq_inc()`, for each of the two clamp IDs (UCLAMP_MIN and UCLAMP_MAX), the code performs: (1) reading `p->uclamp[clamp_id]` to get the task's effective clamp value, (2) indexing into `rq->uclamp[clamp_id].bucket[bucket_id]` to increment the bucket's task count, (3) comparing and potentially updating the bucket's max value, and (4) comparing and potentially updating `uc_rq->value` via `WRITE_ONCE`. In `uclamp_rq_dec()`, the reverse operation decrements refcounts and may trigger `uclamp_rq_max_value()` to scan all buckets for the new maximum.

Similarly, `uclamp_rq_util_with()` in `kernel/sched/sched.h` always reads `rq->uclamp[UCLAMP_MIN].value` and `rq->uclamp[UCLAMP_MAX].value` and performs clamping arithmetic, even when the values are the identity (min=0, max=SCHED_CAPACITY_SCALE) and produce no actual clamping effect.

In `kernel/sched/cpufreq_schedutil.c`, the `schedutil_cpu_util()` function also checks `IS_BUILTIN(CONFIG_UCLAMP_TASK)` to decide whether to apply RT boosting via uclamp — a compile-time check that always evaluates to true when uclamp is compiled in, even if no user has configured any clamping.

All these operations touch additional cache lines (the `uclamp_rq` and `uclamp_se` structures, the bucket arrays) on every enqueue/dequeue/util-calculation, increasing cache pressure. On systems with high task wakeup rates (like netperf UDP processing), this cache pollution accumulates into measurable throughput loss. The exact magnitude was system-specific and appeared related to code/data layout and cache geometry, making it difficult to pinpoint a single hot instruction.

## Consequence

The observable impact is a performance regression of approximately 1.5–3% on network-intensive workloads (netperf UDP_STREAM) and potentially other workloads with high scheduler fast-path activity. This was measured on a dual-socket Xeon E5 system.

The perf profiling data showed that `try_to_wake_up()` consumed more CPU time on the uclamp-enabled kernel: the overhead in `activate_task` increased by +0.39% and `deactivate_task` by +0.38% when uclamp accounting was active. While these percentages seem small, for a function called millions of times per second in a networking workload, the cumulative effect was significant.

There is no crash, hang, data corruption, or incorrect scheduling decision. The scheduler produces identical task placement and frequency selection results whether or not the uclamp fast-path code executes, because the default uclamp values (min=0, max=SCHED_CAPACITY_SCALE) result in identity clamping. The bug is purely a performance issue from unnecessary cache line accesses and instruction execution.

## Fix Summary

The fix introduces a `DEFINE_STATIC_KEY_FALSE(sched_uclamp_used)` static key that gates all uclamp fast-path code. When the static key is disabled (the default), the entire uclamp logic in `uclamp_rq_inc()`, `uclamp_rq_dec()`, and `uclamp_rq_util_with()` is skipped via early returns. The compiler converts the static key check into a NOP instruction that is patched to a jump only when the key is enabled, providing truly zero overhead when uclamp is unused.

The static key is permanently enabled (one-way flip, no disable) when userspace first uses uclamp through any of three paths: (1) a task calls `sched_setattr()` with `SCHED_FLAG_UTIL_CLAMP` flags (in `__setscheduler_uclamp()`), (2) an admin writes to `/proc/sys/kernel/sched_util_clamp_{min,max}` (in `sysctl_sched_uclamp_handler()`), or (3) an admin writes to a cgroup's `cpu.uclamp.{min,max}` (in `cpu_uclamp_write()`). Once enabled, the key stays on until reboot.

The fix also handles a new race condition introduced by the static key: if the key is enabled between a task's enqueue and dequeue, `uclamp_rq_dec_id()` would be called for a task that was never accounted by `uclamp_rq_inc_id()`. This is handled by checking `uc_se->active` at the top of `uclamp_rq_dec_id()` and returning early if it's false (meaning no increment was performed). The commit also replaces the compile-time `IS_BUILTIN(CONFIG_UCLAMP_TASK)` check in schedutil with the new runtime `uclamp_is_used()` helper to preserve RT boosting behavior correctly.

## Triggering Conditions

The performance regression manifests under the following conditions:

- **Kernel configuration**: `CONFIG_UCLAMP_TASK=y` must be enabled at compile time. This is the only hard requirement.
- **No userspace uclamp usage**: The regression is most pronounced when uclamp is compiled in but no userspace program or admin has ever used `sched_setattr()` with uclamp flags, modified `sysctl_sched_util_clamp_{min,max}`, or configured cgroup `cpu.uclamp.*` values. In this state, on the buggy kernel, the uclamp code still executes on every enqueue/dequeue despite being a no-op.
- **High scheduler fast-path activity**: The regression is proportional to the rate of task enqueue/dequeue operations. Workloads with frequent wakeups (e.g., network processing, high-frequency timers, producer-consumer patterns) will show the largest regression.
- **System topology**: The regression magnitude is system-dependent and influenced by cache geometry, code layout, and data alignment. It was most clearly demonstrated on a 2-socket Xeon E5 2x10-core system but may vary on other architectures. Different kernel versions showed varying regression magnitudes, suggesting sensitivity to compiler-generated code layout.

The regression is deterministic and always present when the conditions are met — it is not a race condition or timing-dependent bug. It is observable through any throughput benchmark that exercises the scheduler fast path heavily.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for two reasons:

**1. Kernel version too old (pre-v5.15):** The bug was introduced in v5.3-rc1 (commit 69842cba9ace) and fixed in v5.9-rc1 (this commit). kSTEP supports Linux v5.15 and newer only. By v5.15, this fix has long been applied, so no buggy kernel version is available within kSTEP's supported range.

**2. Performance regression, not a correctness bug:** Even if the kernel version were supported, this bug is a pure performance regression. The scheduling decisions, task placement, and frequency selection are identical on both the buggy and fixed kernels. The only difference is execution speed: the buggy kernel touches additional cache lines (uclamp_rq buckets, uclamp_se structures) on every enqueue/dequeue, causing throughput degradation.

kSTEP cannot measure or detect performance regressions because:
- It has no network benchmarking capability (no netperf, no TCP/UDP socket I/O).
- It cannot measure cache behavior (cache misses, cache line contention, cache pollution).
- It cannot measure throughput or latency of real workloads.
- Its scheduling observation primitives (`kstep_output_curr_task()`, `kstep_eligible()`, etc.) check scheduler state and decisions, not performance characteristics.
- QEMU/TCG provides sequentially consistent memory without realistic cache simulation, so cache-related performance effects are absent.

**What would need to change in kSTEP:** To support this class of bugs, kSTEP would need: (1) a real hardware execution environment instead of QEMU (for realistic cache behavior), (2) integration with performance measurement tools (perf counters, throughput metrics), and (3) support for running real network workloads. These are fundamental architectural changes far beyond minor API additions.

**Alternative reproduction methods:** The bug can be reproduced outside kSTEP by:
1. Building two kernels for the same hardware — one with `CONFIG_UCLAMP_TASK=y` and one without (or one at v5.8 and one at v5.9+).
2. Running `netperf -t UDP_STREAM` with various message sizes (64 to 16384 bytes) on a multi-socket system with high core counts.
3. Comparing throughput (Hmean send rates) between the two kernels; the buggy kernel should show ~2-3% lower throughput.
4. Using `perf diff` to confirm increased overhead in `try_to_wake_up()`, `activate_task()`, and `deactivate_task()`.
