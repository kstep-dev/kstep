# Core: balance_switch() Performance Regression in finish_lock_switch()

**Commit:** `ae7927023243dcc7389b2d59b16c09cbbeaecc36`
**Affected files:** kernel/sched/core.c, kernel/sched/sched.h
**Fixed in:** v5.11-rc1
**Buggy since:** v5.11-rc1 development cycle (introduced by `2558aacff858`, "sched/hotplug: Ensure only per-cpu kthreads run during hotplug", merged after v5.10-rc1)

## Bug Description

Commit `2558aacff858` ("sched/hotplug: Ensure only per-cpu kthreads run during hotplug") introduced a mechanism to push non-per-cpu-kthread tasks off a CPU during CPU hotplug. This was part of the preparation for `migrate_disable()` support. The mechanism added a new `balance_flags` field to `struct rq` and a `balance_switch()` function that was called from `finish_lock_switch()` — the critical tail-end of every context switch.

Before that commit, `finish_lock_switch()` called `__balance_callbacks(rq)` directly. The `__balance_callbacks()` function would call `splice_balance_callbacks()`, which tested the `rq->balance_callback` pointer — a test that was almost always false (NULL) in the fast path. After the commit, `finish_lock_switch()` instead called `balance_switch(rq)`, which first checked `rq->balance_flags` (a new `unsigned char` field) and only then dispatched to either `balance_push()` or `__balance_callbacks()`.

The Intel kernel test robot detected a -1.6% regression on the `will-it-scale/sched_yield` benchmark on a 144-thread Intel Xeon Gold 5318H system. Despite the claim in the original commit message that "This replaces the unlikely(rq->balance_callbacks) test at the tail of context_switch with an unlikely(rq->balance_work), the fast path is not affected," the performance data showed otherwise. The additional load of the `balance_flags` field introduced enough overhead in the extremely hot context-switch path to cause a measurable throughput degradation.

The fix commit (`ae7927023243`) by Peter Zijlstra restored `finish_lock_switch()` to its original behavior, replacing the `balance_switch()` call with `__balance_callbacks(rq)`, and solved the hotplug push problem by repurposing the existing `balance_callback` pointer rather than introducing a separate flags field.

## Root Cause

The root cause is an additional memory load in the hot path of every context switch. In the original code before `2558aacff858`, `finish_lock_switch()` was:

```c
static inline void finish_lock_switch(struct rq *rq)
{
    spin_acquire(&rq->lock.dep_map, 0, 0, _THIS_IP_);
    __balance_callbacks(rq);
    raw_spin_unlock_irq(&rq->lock);
}
```

`__balance_callbacks()` calls `splice_balance_callbacks()`, which loads `rq->balance_callback`. In the common case, `rq->balance_callback` is NULL, so `splice_balance_callbacks()` returns NULL immediately, and `do_balance_callbacks()` (called from `__balance_callbacks()`) is a no-op since `head` is NULL. The compiler can potentially optimize this chain.

After `2558aacff858`, `finish_lock_switch()` called `balance_switch(rq)` instead:

```c
static inline void balance_switch(struct rq *rq)
{
    if (likely(!rq->balance_flags))
        return;

    if (rq->balance_flags & BALANCE_PUSH) {
        balance_push(rq);
        return;
    }

    __balance_callbacks(rq);
}
```

Although `rq->balance_flags` is on the same cache line as `rq->balance_callback`, the change introduces a subtly different instruction sequence. The new code loads `rq->balance_flags` (an `unsigned char`) first, tests it, and takes a branch. In the fast path, this is indeed a single load and compare-to-zero, similar to the original `rq->balance_callback` test. However, the actual generated code differs because:

1. The `balance_switch()` function is a separate inline function with different control flow than `__balance_callbacks()`.
2. The compiler may generate different instruction scheduling or register allocation for the new code path.
3. The branch prediction characteristics may differ slightly — the original NULL pointer test in `splice_balance_callbacks()` was deeply inlined and well-predicted, while the new `balance_flags` test in `balance_switch()` represents a different branch prediction entry.

On a 144-thread system running `sched_yield()` in a tight loop with 100% CPU utilization, even a single extra instruction or a slight change in branch prediction accuracy in the context-switch path leads to measurable throughput regression. The `sched_yield` microbenchmark is particularly sensitive because it maximizes the rate of context switches, amplifying any per-switch overhead.

The additional `balance_flags` field also increased the `struct rq` size and potentially affected cache line layout, though both fields were in the same cache line. The removal of `balance_flags` in the fix also slightly reduces memory footprint per CPU.

## Consequence

The observable impact is a -1.6% throughput regression on the `will-it-scale/sched_yield` benchmark. This translates to approximately 45,935 fewer per-thread operations per second (from 2,785,455 down to 2,739,520) across the 144-thread test configuration. While -1.6% may seem small, in the context of the `sched_yield` fast path — which is essentially measuring raw context-switch throughput — this represents a meaningful increase in per-context-switch latency that could affect latency-sensitive workloads.

The regression was also accompanied by secondary effects visible in the benchmark data: a -21.7% reduction in context switches per second (`vmstat.system.cs`), and significant changes in NUMA hit/miss statistics on multi-socket systems. The system-time percentage (`vmstat.cpu.sy`) decreased from 86.25% to 84.25%, while user-time increased, suggesting the scheduler overhead per switch increased and context switches became less frequent but each one took longer.

The impact is limited to performance; there is no crash, data corruption, or incorrect scheduling behavior. The regression would be most noticeable on workloads that perform extremely frequent voluntary context switches (e.g., message-passing systems, fiber schedulers, or cooperative multitasking frameworks) on high-core-count machines. The effect scales with CPU count and context-switch rate.

## Fix Summary

The fix commit restores `finish_lock_switch()` to call `__balance_callbacks(rq)` directly, exactly as it was before `2558aacff858`. This eliminates the `balance_switch()` function and the `balance_flags` field entirely:

```c
static inline void finish_lock_switch(struct rq *rq)
{
    spin_acquire(&rq->lock.dep_map, 0, 0, _THIS_IP_);
    __balance_callbacks(rq);    /* restored from balance_switch(rq) */
    raw_spin_unlock_irq(&rq->lock);
}
```

To preserve the CPU hotplug push functionality without the flags field, the fix introduces a global `struct callback_head balance_push_callback` that points to `balance_push()`. When hotplug push is enabled (`balance_push_set(cpu, true)`), `rq->balance_callback` is set to `&balance_push_callback` instead of setting `rq->balance_flags |= BALANCE_PUSH`. The `balance_push()` function itself re-installs this callback at the start of each invocation (`rq->balance_callback = &balance_push_callback`), making the push mechanism persistent until explicitly disabled.

The `queue_balance_callback()` function is updated to check `rq->balance_callback == &balance_push_callback` instead of `rq->balance_flags & BALANCE_PUSH` to determine whether to skip queuing regular balance callbacks. The `rq_pin_lock()` debug assertion is updated to allow `rq->balance_callback` to be non-NULL if it equals `&balance_push_callback` (since this callback is persistent during hotplug). The `splice_balance_callbacks()` function is simplified to just clear `rq->balance_callback` without needing to clear any flags.

This approach is correct and complete because it achieves the same functional behavior — persistent balance_push during hotplug, priority over normal balance callbacks, and no interference with the normal balance callback mechanism — while eliminating the extra field load from the fast path. The context-switch hot path now contains exactly the same instructions as before `2558aacff858`, resolving the performance regression.

## Triggering Conditions

This is a performance regression, not a correctness bug. To observe the regression:

- **Hardware**: A high-core-count machine is needed. The original report used a 144-thread Intel Xeon Gold 5318H (4-socket). The regression is amplified with more cores because more context switches happen system-wide.
- **Workload**: The `will-it-scale/sched_yield` benchmark at 100% CPU utilization (`nr_task: 100%`, `mode: thread`, `test: sched_yield`). This creates one thread per logical CPU, each calling `sched_yield()` in a tight loop to maximize context-switch throughput.
- **Configuration**: `cpufreq_governor: performance` to eliminate frequency scaling noise. No special kernel configuration beyond `CONFIG_SMP=y` is needed.
- **Measurement**: Precise, low-noise performance measurement infrastructure (such as Intel's LKP/0-day test infrastructure) is required to reliably detect a 1.6% regression.
- **Kernel version**: The regression exists only in kernels containing commit `2558aacff858` but not `ae7927023243`. Since both were merged into v5.11-rc1, the regression only existed in development/tip trees, not in any released stable kernel.

The regression is deterministic and reproducible given sufficient measurement precision, but requires statistical analysis across multiple runs to distinguish from noise, as the 1.6% signal is close to the noise floor of typical benchmark environments.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for two independent reasons:

### 1. Kernel Version Too Old

The fix targets the v5.10-rc/v5.11-rc development cycle. Both the buggy commit (`2558aacff858`) and the fix (`ae7927023243`) were merged into v5.11-rc1. kSTEP supports Linux v5.15 and newer only. The code structure around `finish_lock_switch()`, `balance_switch()`, and `balance_flags` was specific to this narrow window of the v5.10-rc to v5.11-rc1 development cycle. By v5.15, the code already includes the fix.

### 2. Performance Regression Requires Real Hardware Benchmarking

Even if the kernel version were supported, this bug is a **performance regression**, not a correctness bug. kSTEP is designed to reproduce functional/correctness bugs (crashes, incorrect scheduling decisions, priority inversions, starvation, data corruption). The -1.6% throughput regression on `sched_yield` can only be detected by:

- Running a real `will-it-scale/sched_yield` benchmark workload with real user-space threads calling `sched_yield()` in a tight loop.
- Measuring wall-clock throughput (operations per second) with statistical rigor across multiple runs.
- Having a high-core-count system (144 threads in the original report) to amplify the per-context-switch overhead into a measurable aggregate effect.
- Using precise performance counters (`perf-stat`, etc.) to observe instruction-level differences in the context-switch path.

kSTEP cannot intercept userspace syscalls directly, cannot run real `sched_yield()` workloads, and does not have performance measurement infrastructure. The bug manifests as a subtle change in generated machine code and branch prediction behavior, not as any observable difference in scheduler state or scheduling decisions.

### 3. What Would Be Needed to Support This

To reproduce this class of bug, kSTEP would need fundamental additions:
- **Real userspace process support**: Ability to run actual userspace programs (like `will-it-scale`) inside the QEMU VM and measure their throughput.
- **Performance measurement infrastructure**: High-precision timing, operation counters, and statistical analysis to detect small (1-2%) throughput regressions.
- **High CPU count**: The QEMU configuration would need many virtual CPUs (ideally 32+) to amplify the signal.
- **Benchmark harness integration**: Integration with benchmark suites like `will-it-scale` or at minimum a framework for measuring context-switch throughput.

These are fundamental architectural changes far beyond minor kSTEP extensions.

### 4. Alternative Reproduction Methods

Outside kSTEP, the bug can be reproduced by:
1. Building the kernel at commit `2558aacff858` (contains the regression).
2. Building the kernel at commit `ae7927023243` or `2558aacff858~1` (without the regression).
3. Running the `will-it-scale/sched_yield` benchmark on both kernels on a high-core-count machine.
4. Comparing per-thread operations per second across multiple runs with statistical significance testing.
5. The Intel LKP (Linux Kernel Performance) test infrastructure can reproduce this automatically using the job file attached to the original report email.
