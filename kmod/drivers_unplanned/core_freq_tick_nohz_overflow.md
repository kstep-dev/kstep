# Core: arch_scale_freq_tick() overflow on tickless (nohz_full) CPUs

**Commit:** `7fb3ff22ad8772bbf0e3ce1ef3eb7b09f431807f`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.2-rc4
**Buggy since:** v5.9-rc1 (commit `e2b0d619b400` introduced the overflow check that triggers the error path)

## Bug Description

The Linux scheduler uses frequency invariant accounting to normalize task utilization measurements across different CPU frequencies. On x86, this works by reading the APERF and MPERF hardware MSR counters at every scheduler tick to compute the ratio of actual CPU frequency to maximum CPU frequency. The delta between consecutive APERF readings is shifted left by `2 * SCHED_CAPACITY_SHIFT` (20 bits) and divided by the MPERF delta multiplied by `arch_max_freq_ratio` to produce a frequency scale factor.

On systems configured with `CONFIG_NO_HZ_FULL` (full tickless mode), certain CPUs designated as `nohz_full` can go extended periods without receiving scheduler ticks — potentially tens of minutes or even hours. During these long tickless periods, the APERF and MPERF hardware counters continue to accumulate, causing the delta between the last-recorded counter value and the current counter value to grow extremely large.

When a scheduler tick finally fires on such a CPU (e.g., due to a second task being scheduled or a transition out of tickless mode), `scheduler_tick()` calls `arch_scale_freq_tick()`, which computes the APERF/MPERF delta. The accumulated delta is so large that the left-shift by 20 bits overflows `u64`, triggering the `check_shl_overflow()` safety check. This causes the error path to execute, which prints the warning "Scheduler frequency invariance went wobbly, disabling!" and globally disables the frequency invariance feature for ALL CPUs via `static_branch_disable(&arch_scale_freq_key)`.

The bug was reported by Yair Podemsky at Red Hat, who observed the overflow happening after approximately 30 minutes of tickless operation on systems running at 5GHz. The issue particularly affects latency-sensitive workloads that use `nohz_full` isolation (e.g., DPDK, real-time applications) where CPUs intentionally avoid ticks for long durations.

## Root Cause

The root cause is that `scheduler_tick()` unconditionally calls `arch_scale_freq_tick()` on every CPU, including those designated as `nohz_full` CPUs that may have been tickless for extended periods.

In `kernel/sched/core.c`, the `scheduler_tick()` function begins with:

```c
void scheduler_tick(void)
{
    int cpu = smp_processor_id();
    ...
    arch_scale_freq_tick();  /* Called unconditionally */
    ...
}
```

On x86, `arch_scale_freq_tick()` is implemented in `arch/x86/kernel/cpu/aperfmperf.c` and does the following:

```c
void arch_scale_freq_tick(void)
{
    struct aperfmperf *s = this_cpu_ptr(&cpu_samples);
    u64 acnt, mcnt, aperf, mperf;

    rdmsrl(MSR_IA32_APERF, aperf);
    rdmsrl(MSR_IA32_MPERF, mperf);
    acnt = aperf - s->aperf;     /* Delta since last tick */
    mcnt = mperf - s->mperf;
    s->aperf = aperf;
    s->mperf = mperf;
    ...
    scale_freq_tick(acnt, mcnt);
}
```

The `scale_freq_tick()` function then performs:

```c
static void scale_freq_tick(u64 acnt, u64 mcnt)
{
    if (check_shl_overflow(acnt, 2*SCHED_CAPACITY_SHIFT, &acnt))
        goto error;
    if (check_mul_overflow(mcnt, arch_max_freq_ratio, &mcnt) || !mcnt)
        goto error;
    ...
error:
    pr_warn("Scheduler frequency invariance went wobbly, disabling!\n");
    schedule_work(&disable_freq_invariance_work);
}
```

The overflow check `check_shl_overflow(acnt, 2*SCHED_CAPACITY_SHIFT, &acnt)` shifts `acnt` left by 20 bits. For this to overflow `u64`, `acnt` must exceed `2^44 ≈ 1.76 × 10^13`. The APERF counter increments approximately once per CPU cycle. At 5 GHz, the counter increments at `5 × 10^9` per second, so a tickless period of `2^44 / (5 × 10^9) ≈ 3518 seconds ≈ 58.6 minutes` would cause the overflow. In practice, the overflow was observed after approximately 30 minutes due to variations in CPU frequency and counting behavior.

The fundamental issue is that frequency invariant accounting is meaningless for CPUs that have been tickless for extended periods — the accumulated delta does not represent a useful instantaneous frequency measurement. The counters were designed to be sampled at regular tick intervals (every 1ms to 10ms), not after minutes of silence.

## Consequence

When the overflow is detected, the kernel prints the warning:

```
Scheduler frequency invariance went wobbly, disabling!
```

This triggers `schedule_work(&disable_freq_invariance_work)` which calls `static_branch_disable(&arch_scale_freq_key)`, globally disabling frequency invariant accounting for **all CPUs in the system**, not just the offending nohz_full CPU. This means all subsequent scheduler decisions across the entire system lose the ability to account for CPU frequency differences.

The loss of frequency invariance has significant performance implications. Without it, the scheduler's PELT (Per-Entity Load Tracking) calculations cannot properly normalize utilization across CPUs running at different frequencies. This leads to incorrect load balancing decisions, suboptimal task placement, and degraded performance for frequency-sensitive workloads. The schedutil cpufreq governor also relies on frequency invariant signals to make accurate frequency selection decisions — without invariance, it may set inappropriate CPU frequencies.

Additionally, once disabled, frequency invariance cannot be re-enabled without a reboot. The `static_branch_disable()` call is permanent for the lifetime of the system. This means a single long tickless period on one nohz_full CPU permanently degrades scheduling quality for the entire machine, which is particularly ironic since nohz_full systems are typically performance-sensitive environments.

## Fix Summary

The fix adds a simple guard in `scheduler_tick()` to skip the `arch_scale_freq_tick()` call on CPUs that are not designated as tick housekeeping CPUs. The change in `kernel/sched/core.c` replaces:

```c
arch_scale_freq_tick();
```

with:

```c
if (housekeeping_cpu(cpu, HK_TYPE_TICK))
    arch_scale_freq_tick();
```

The `housekeeping_cpu(cpu, HK_TYPE_TICK)` check returns `true` for CPUs that are expected to receive regular ticks, and `false` for CPUs in the `nohz_full` set that may go tickless for extended periods. By only calling `arch_scale_freq_tick()` on housekeeping CPUs, the fix ensures that the APERF/MPERF delta computation only occurs on CPUs where the tick interval is bounded and the delta values will remain within the safe range for the overflow checks.

This approach was suggested by Peter Zijlstra in the V1 review thread. His reasoning was that nohz_full CPUs typically don't need frequency invariant accounting because they don't participate in load balancing and often don't use DVFS. Valentin Schneider noted that nohz_full CPUs aren't always tickless (they transition between tickless and ticking states), but the guard still prevents the overflow because the stale counter values from tickless periods are simply skipped rather than processed. The next tick after a tickless period on a non-housekeeping CPU will not call `arch_scale_freq_tick()`, so no overflow can occur. This is a V2 solution; the V1 approach attempted to detect long tickless periods directly but was deemed overly complex.

## Triggering Conditions

The following conditions must all be met to trigger the bug:

- **Architecture**: x86_64 with APERF/MPERF MSR support (`X86_FEATURE_APERFMPERF`). These are Intel and AMD CPUs that support the APERF and MPERF model-specific registers for frequency measurement.
- **Kernel configuration**: `CONFIG_NO_HZ_FULL=y` must be enabled to allow full tickless operation. Additionally, `CONFIG_SMP=y` and `CONFIG_X86_64=y` are required for the frequency invariant code path to be compiled.
- **Boot parameters**: `nohz_full=<cpulist>` must be specified on the kernel command line to designate specific CPUs as tickless. These CPUs must not be in the `HK_TYPE_TICK` housekeeping set.
- **Frequency invariance enabled**: The `arch_scale_freq_key` static branch must be enabled, which requires successful initialization of `arch_max_freq_ratio` during boot (Intel or AMD frequency detection must succeed).
- **Workload pattern**: A nohz_full CPU must run a single task (or be idle) for an extended period (~30-60 minutes at 5GHz) without receiving a scheduler tick. This is the normal operating mode for latency-sensitive workloads using CPU isolation.
- **Tick resumption**: After the long tickless period, the CPU must receive a scheduler tick. This happens when a second task is enqueued on the CPU, or when the CPU transitions out of nohz_full mode for any reason.

The reproduction is highly reliable on affected configurations — any nohz_full CPU that goes tickless for long enough will deterministically trigger the overflow. The exact time depends on CPU frequency: at 5GHz it takes ~30 minutes (as reported), at 3GHz it would take ~50 minutes, and at 1GHz it would take ~150 minutes. The `check_shl_overflow()` and `check_mul_overflow()` checks are deterministic once the counter delta exceeds the threshold.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

**1. Requires x86 APERF/MPERF hardware MSR counters.** The bug is triggered inside `arch_scale_freq_tick()` which reads `MSR_IA32_APERF` and `MSR_IA32_MPERF` via `rdmsrl()`. These are x86-specific model-specific registers that track CPU cycles in active and maximum performance states. kSTEP runs inside QEMU, and in TCG (software emulation) mode, QEMU does not emulate APERF/MPERF MSRs — `rdmsrl()` would either return zero or trap. Even with KVM passthrough, the MSR values depend on real host hardware behavior and cannot be controlled or manipulated by a kernel module. Without valid APERF/MPERF counter values, the entire `arch_scale_freq_tick()` function either returns early (if `!cpu_feature_enabled(X86_FEATURE_APERFMPERF)`) or produces meaningless deltas.

**2. Requires CONFIG_NO_HZ_FULL with actual tickless CPU behavior.** The overflow requires a CPU to accumulate a very large APERF delta by running without scheduler ticks for tens of minutes. kSTEP's tick control API (`kstep_tick()`, `kstep_tick_repeat()`) drives ticks explicitly — it cannot suppress ticks on a specific CPU for extended periods while that CPU's hardware counters continue accumulating. The nohz_full infrastructure requires specific boot-time configuration (`nohz_full=<cpulist>`) and depends on the kernel's internal tick suppression logic, which kSTEP cannot control from module space.

**3. The overflow requires real time passage and counter accumulation.** Even if APERF/MPERF counters were available, the overflow requires ~30-60 minutes of real wall-clock time for the delta to grow large enough. kSTEP's `kstep_tick()` advances the scheduler tick counter but does not advance real hardware counters. The APERF/MPERF registers are incremented by actual CPU cycles, not by software-driven tick events. There is no way to artificially inflate these hardware counters from a kernel module.

**4. Cannot inject fake APERF/MPERF values.** One might consider writing to the per-CPU `arch_prev_aperf`/`arch_prev_mperf` fields (stored in the `struct aperfmperf cpu_samples` per-CPU variable) to simulate a large delta. However, in the kernel version around this fix (v6.1-v6.2), these fields are accessed via `this_cpu_ptr(&cpu_samples)` and would need to be manipulated on the specific target CPU. Even if kSTEP used `KSYM_IMPORT` to access `cpu_samples`, writing a fake previous value would only work if the current `rdmsrl()` returns a much larger value — which it won't in QEMU without real MSR support.

**5. The fix is architecture-specific with no scheduler-internal state to observe.** The bug manifests as a `pr_warn()` message and a `static_branch_disable()` call. There is no scheduler-internal state change (like incorrect task placement or runqueue corruption) that could be observed through kSTEP's observation APIs. The consequence is a global disabling of frequency invariance, which would require monitoring `arch_scale_freq_invariant()` return value or `per_cpu(arch_freq_scale, cpu)` values over time — but these would be meaningless in QEMU without real frequency scaling hardware.

**What would need to be added to kSTEP:** Reproducing this bug would require fundamental additions far beyond kSTEP's architecture: (a) MSR emulation or interception support in QEMU to provide controllable APERF/MPERF values, (b) ability to configure nohz_full boot parameters for the QEMU guest, (c) real-time clock-driven counter accumulation over 30+ minute periods, and (d) the ability to suppress ticks on specific CPUs for extended durations. These are not minor extensions — they require hardware emulation capabilities and boot-time kernel configuration.

**Alternative reproduction methods:** This bug can be reproduced on bare-metal x86_64 hardware by: (1) booting with `nohz_full=<cpu>` where `<cpu>` is a CPU that supports APERF/MPERF; (2) pinning a single CPU-intensive task to that CPU; (3) waiting 30-60 minutes; (4) monitoring `dmesg` for the "went wobbly" message. The V1 patch notes confirm this was reproduced in under 30 minutes on systems running at 5GHz.
