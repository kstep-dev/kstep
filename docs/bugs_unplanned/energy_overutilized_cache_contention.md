# Energy: Unnecessary overutilized Writes Cause Cache Contention on Non-EAS Systems

**Commit:** `be3a51e68f2f1b17250ce40d8872c7645b7a2991`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.10-rc1
**Buggy since:** v4.20-rc5 (commit `2802bf3cd936` "sched/fair: Add over-utilization/tipping point indicator")

## Bug Description

The `root_domain::overutilized` field in the scheduler is used exclusively by the Energy Aware Scheduler (EAS) to decide whether to perform EAS-aware load balancing or fall back to regular load balancing. When EAS is not enabled (which is the case on all non-asymmetric-capacity platforms, i.e., most server and desktop systems), this field serves no purpose. However, the original code unconditionally accessed and updated `rd->overutilized` in three hot scheduler paths: `enqueue_task_fair()`, `task_tick_fair()`, and `update_sd_lb_stats()`.

On large multi-CPU systems (the report came from a 240-core, SMT8 IBM Power system), this unnecessary read-modify-write pattern on `rd->overutilized` caused severe cache contention. Every scheduler tick on every CPU would call `update_overutilized_status()` which reads `rq->rd->overutilized`. Every task enqueue similarly accessed the field. In `update_sd_lb_stats()`, the field was written on every load balancing pass at the root domain level. Since all CPUs in a root domain share the same `struct root_domain`, all these operations target the same cache line.

The problem was compounded by false sharing: `root_domain::overload` and `root_domain::overutilized` reside in the same cache line. The `overload` field is actually used on non-EAS systems (in `newidle_balance()` to decide whether to pull tasks). Frequent writes to `overutilized` by multiple CPUs invalidated the cache line containing `overload`, causing accesses to `overload` in `newidle_balance()` to stall waiting for the cache line to be fetched from a remote CPU's cache. This turned `newidle_balance()` into a hot function consuming ~6.78% of total CPU time.

The issue was first observed with an ISV workload and then reproduced using a modified `stress-ng --wait` benchmark. On the affected 240-core system, `enqueue_task_fair` consumed 7.18% and `newidle_balance` consumed 6.78% of total CPU time, with perf annotate showing that nearly all cycles were spent on the load of `rd` and `rd->overutilized`.

## Root Cause

The root cause is that `update_overutilized_status()` (called from `enqueue_task_fair()` and `task_tick_fair()`) and direct writes to `rd->overutilized` (in `update_sd_lb_stats()`) were unconditional — they executed regardless of whether EAS was enabled.

In `enqueue_task_fair()`, after adding a waking task to the runqueue, the code called:
```c
if (!task_new)
    update_overutilized_status(rq);
```
The `update_overutilized_status()` function read `rq->rd->overutilized` via `READ_ONCE()`, then if zero, called `cpu_overutilized()` which performed expensive utilization calculations (accessing `uclamp_rq_get()`, `cpu_util_cfs()`, `util_fits_cpu()`), and finally wrote `SG_OVERUTILIZED` back via `WRITE_ONCE()`. On non-EAS platforms, this was pure waste.

In `task_tick_fair()`, every scheduler tick executed:
```c
update_overutilized_status(task_rq(curr));
```
This means every CPU on every tick was reading the shared `rd->overutilized` field, causing a cache line fetch even if no write occurred. On a 240-core system, this is 240 reads per tick period (typically 4ms), constantly bouncing the cache line.

In `update_sd_lb_stats()`, at the root scheduling domain level, the code unconditionally wrote:
```c
WRITE_ONCE(rd->overutilized, sg_status & SG_OVERUTILIZED);
```
This write happened on every load balancing pass at the root domain, which occurs frequently. Since this is a write (not just a read), it invalidated the cache line on all other CPUs, forcing them to re-fetch it next time they access either `overutilized` or `overload`.

The critical performance impact came from the cache line layout of `struct root_domain`. The `overload` field (at offset 536 in the Power assembly shown in the perf annotate) and `overutilized` field (at offset 540) are adjacent and share the same 64-byte or 128-byte cache line. When `update_sd_lb_stats()` writes `overutilized`, it invalidates the cache line. When `newidle_balance()` subsequently reads `overload` (which it legitimately needs), it must wait for the cache line to be fetched from whichever remote CPU last modified it. On a large NUMA system with many CPUs, this fetch latency is substantial (potentially hundreds of nanoseconds), and it happens on every idle balance attempt.

The perf annotate data from the Power system is telling: in `enqueue_task_fair()`, the load instruction `ld r8,2752(r28)` (loading `rq->rd`) took 95.42% of the function's cycles. This is because the load stalls waiting for the cache line to arrive from a remote cache. Similarly, in `newidle_balance()`, loading `rd` took 41.54% of the function's cycles.

## Consequence

The observable impact is severe performance degradation on large multi-CPU non-EAS systems. The bug wastes CPU cycles in two ways:

1. **True sharing of `overutilized`**: Multiple CPUs simultaneously read and write `rd->overutilized` in `enqueue_task_fair()`, `task_tick_fair()`, and `update_sd_lb_stats()`. Each write invalidates the cache line on all other CPUs. The resulting cache coherency traffic consumes significant cycles. On the 240-core Power system, `enqueue_task_fair` alone consumed 7.18% of total CPU time, almost entirely spent stalling on the `rd` pointer load.

2. **False sharing with `overload`**: The `overload` field shares a cache line with `overutilized`. `newidle_balance()` reads `overload` to decide whether to attempt task migration. The unnecessary writes to `overutilized` cause `newidle_balance()` to stall on loading `overload`, consuming 6.78% of total CPU time. This is particularly damaging because idle balancing is the mechanism by which idle CPUs pull work — slowing it down increases scheduling latency and reduces system throughput.

The combined effect was that ~14% of total CPU time on the 240-core system was wasted purely on cache coherency stalls in scheduler hot paths. An ISV workload experienced measurable throughput degradation. The severity scales with CPU count: on smaller systems (e.g., 8-16 cores), the cache coherency domain is smaller and the penalty is less. On very large systems with deep NUMA hierarchies, the penalty is amplified because cache line fetches must traverse inter-socket interconnects.

There is no crash, hang, or incorrect scheduling behavior — the `overutilized` field was always set to a valid value. The bug is purely a performance issue caused by unnecessary shared-state writes.

## Fix Summary

The fix adds `sched_energy_enabled()` checks before any access to or update of `rd->overutilized`. The `sched_energy_enabled()` function checks a static key (`sched_energy_present`) that is set only when EAS is active. On non-EAS systems, this is optimized at compile time (if `CONFIG_ENERGY_MODEL` is not set) or resolved via a static branch (essentially a NOP in the fast path).

Specifically, the fix introduces two new helper functions:

1. `set_rd_overutilized_status(rd, status)` — wraps `WRITE_ONCE(rd->overutilized, status)` and `trace_sched_overutilized_tp()` behind a `sched_energy_enabled()` check. It also correctly converts the status to `bool` for the tracepoint argument (fixing a minor type mismatch where `trace_sched_overutilized_tp` expected `bool` but received `unsigned int`).

2. `check_update_overutilized_status(rq)` — replaces the old `update_overutilized_status()`. It first checks `sched_energy_enabled()` and returns immediately if EAS is not active. Only if EAS is enabled does it read `rd->overutilized` and potentially call `cpu_overutilized()`.

The fix also adds an early return to `cpu_overutilized()` itself: if `!sched_energy_enabled()`, it returns `false` immediately, avoiding the expensive `uclamp_rq_get()` and `util_fits_cpu()` calculations.

In `update_sd_lb_stats()`, the direct `WRITE_ONCE(rd->overutilized, ...)` calls are replaced with `set_rd_overutilized_status()`, which gates the write behind the EAS check. The `rd->overload` write remains unconditional since it is legitimately needed on all systems.

The result: on non-EAS platforms, none of the overutilized-related code executes at all. The cache line containing `overload` and `overutilized` is no longer bounced by `overutilized` writes, so `newidle_balance()`'s access to `overload` becomes fast. The perf numbers confirm: after the fix, `enqueue_task_fair` dropped from 7.18% to 0.14% and `newidle_balance` from 6.78% to 0.00% of CPU time.

## Triggering Conditions

The bug is triggered under the following conditions:

- **Non-EAS platform**: The system must not have EAS enabled. EAS requires `CONFIG_ENERGY_MODEL=y`, an asymmetric CPU capacity topology (e.g., big.LITTLE), and a registered energy model. On symmetric multiprocessor systems (all CPUs have equal capacity), EAS is never enabled, so `sched_energy_enabled()` returns false. This includes essentially all x86 server/desktop systems and symmetric Power/ARM servers.

- **Multiple CPUs**: The severity scales with CPU count. The bug is most impactful on large systems (100+ cores) but exists on any SMP system. The 240-core SMT8 Power system where it was first observed is an extreme case. On smaller systems (4-16 cores), the cache coherency cost is lower but still non-zero.

- **Active workload with frequent wakeups**: The cache contention is driven by `enqueue_task_fair()` (called on every task wakeup) and `task_tick_fair()` (called on every scheduler tick for CFS tasks). Workloads with many short-sleeping tasks that frequently wake up and sleep (like the `stress-ng --wait` workload used to reproduce) maximize the write rate to the shared cache line.

- **Frequent idle balancing**: The false sharing impact on `newidle_balance()` is amplified when CPUs frequently go idle and attempt to steal work. This happens with bursty workloads where CPUs oscillate between busy and idle states.

- **No special kernel configuration**: The bug exists with default kernel configuration on any SMP system without EAS. `CONFIG_SMP=y` is sufficient. The code paths are always compiled in when `CONFIG_SMP` is set.

- **No race condition or timing sensitivity**: This is not a race condition bug. The cache contention occurs deterministically whenever the affected code paths execute on multiple CPUs. The severity depends on the rate of execution and the number of CPUs, not on specific timing.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **The bug is a pure performance issue, not a correctness bug.** The `overutilized` field is always set to a correct value — the problem is that it is set at all on non-EAS systems, causing unnecessary cache line bouncing. There is no incorrect scheduling decision, no crash, no hang, no priority inversion, no starvation — only wasted CPU cycles. kSTEP is designed to observe scheduler behavior (task placement, runqueue state, scheduling decisions), not hardware cache performance characteristics.

2. **Cache contention cannot be observed in QEMU.** kSTEP runs inside QEMU, which emulates a CPU but does not simulate real cache coherency hardware. QEMU does not model L1/L2/L3 caches, cache lines, MESI/MOESI protocol states, or cache line invalidation latencies. The key symptom — stalling on `ld r8,2752(r28)` for hundreds of nanoseconds waiting for a cache line fetch — simply does not occur in QEMU. All memory accesses in QEMU complete in approximately the same time regardless of sharing patterns.

3. **The bug requires a large number of CPUs to manifest meaningfully.** While QEMU can technically be configured with many vCPUs, the cache contention that makes this bug severe is a property of real hardware with real NUMA interconnects and cache hierarchies. In QEMU, adding more vCPUs does not produce the inter-socket cache transfer latencies that cause the performance degradation.

4. **There is no observable scheduling behavior difference.** On a buggy kernel, tasks are scheduled identically to a fixed kernel — the `overutilized` field's value has no effect on non-EAS systems because it is never read for scheduling decisions when EAS is disabled. kSTEP's observation primitives (`kstep_output_curr_task()`, `kstep_output_balance()`, etc.) would show identical results on buggy and fixed kernels.

5. **No kSTEP extension could help.** Even if kSTEP were extended with cycle counting or perf-counter reading capabilities, QEMU does not provide accurate cycle counts that reflect cache contention. The perf counters in QEMU are either unavailable or do not model real hardware behavior. Reproducing this bug fundamentally requires real multi-socket hardware with real cache coherency overhead.

**What would need to be added to kSTEP to support this (hypothetically):**
- A hardware performance counter interface (e.g., `kstep_read_perf_counter(cpu, counter)`) that returns accurate cache miss counts, stall cycles, or similar metrics. However, this would only be meaningful on real hardware, not in QEMU.
- Alternatively, kSTEP could instrument the kernel to count calls to `update_overutilized_status()` or `set_rd_overutilized_status()` and verify that the count drops to zero on the fixed kernel when EAS is not enabled. However, this would be testing the presence of the fix, not reproducing the bug's observable impact.

**Alternative reproduction methods outside kSTEP:**
1. **Bare-metal testing**: Run `stress-ng --wait` (or a workload with many short-sleeping tasks) on a large multi-socket system (preferably 100+ cores). Use `perf record -g` and `perf report` to check if `enqueue_task_fair` and `newidle_balance` appear as hot functions. Use `perf annotate` to verify that the stall is on the `rd->overutilized` access. Compare buggy vs. fixed kernel profiles.
2. **perf c2c analysis**: Use `perf c2c record` and `perf c2c report` on a buggy kernel to directly identify the cache line containing `overload`/`overutilized` as a contended line. This tool is specifically designed to detect true/false sharing.
3. **lockstat or tracing**: While not directly applicable (there's no lock), one could add tracepoints around the `overutilized` access to count the frequency of reads/writes and correlate with system throughput.
