# Core: local_clock() returns incorrect timestamps before sched_clock_init()

**Commit:** `f31dcb152a3d0816e2f1deab4e64572336da197d`
**Affected files:** kernel/sched/clock.c
**Fixed in:** v6.4-rc1
**Buggy since:** v6.3-rc1 (commit 776f22913b8e "sched/clock: Make local_clock() noinstr")

## Bug Description

When the kernel's `local_clock()` function was refactored in commit 776f22913b8e to be marked `noinstr` (no instrumentation, for safe use in noinstr regions like entry code and NMIs), a critical early-boot guard check was omitted from the new implementation. The original `local_clock()` was a simple inline wrapper around `sched_clock_cpu(raw_smp_processor_id())`, which internally checked whether the scheduler clock subsystem had been initialized via the `sched_clock_running` static branch. The new standalone `noinstr` implementation of `local_clock()` in `kernel/sched/clock.c` skipped this check and went straight to calling `sched_clock_local()` when the clock was not stable.

During early boot, before `sched_clock_init()` runs, the per-CPU `sched_clock_data` structures have not been properly initialized. Specifically, `scd->tick_raw`, `scd->tick_gtod`, and `scd->clock` are all zero (or at their default values). When `sched_clock_local()` operates on these uninitialized values, it computes a `max_clock` value clamped to `gtod + TICK_NSEC` (where `gtod` is effectively `__gtod_offset` since `scd->tick_gtod` is zero). The result is that `scd->clock` rapidly reaches the value of `TICK_NSEC` (typically 1,000,000 ns = 1 ms) and then stays stuck at that value until `sched_clock_init()` runs and properly initializes the per-CPU data.

This causes all early boot timestamps obtained via `local_clock()` to show the same value of approximately 0.001000 seconds, making it impossible to determine the actual timing of early boot events. The existing `sched_clock_cpu()` function, which is the other primary interface to the scheduler clock, correctly includes the `sched_clock_running` guard and falls back to raw `sched_clock()` during early boot. The bug is simply that the new `local_clock()` implementation failed to replicate this same guard.

## Root Cause

The root cause is a missing early-boot guard in the `noinstr` implementation of `local_clock()` added by commit 776f22913b8e. The function was implemented as:

```c
noinstr u64 local_clock(void)
{
    u64 clock;

    if (static_branch_likely(&__sched_clock_stable))
        return sched_clock() + __sched_clock_offset;

    preempt_disable_notrace();
    clock = sched_clock_local(this_scd());
    preempt_enable_notrace();

    return clock;
}
```

Compare this with `sched_clock_cpu()` which includes the critical guard:

```c
notrace u64 sched_clock_cpu(int cpu)
{
    struct sched_clock_data *scd;
    u64 clock;

    if (sched_clock_stable())
        return sched_clock() + __sched_clock_offset;

    if (!static_branch_likely(&sched_clock_running))  /* <-- THIS CHECK IS MISSING */
        return sched_clock();

    preempt_disable_notrace();
    scd = cpu_sdc(cpu);
    ...
}
```

The `sched_clock_running` static key is enabled by `sched_clock_init()`, which runs during kernel initialization (called from `sched_init()` or later). Before this point, the per-CPU `sched_clock_data` structures are in their default zero-initialized state. When `sched_clock_local()` is called with this uninitialized data, it computes:

1. `now = sched_clock()` — returns the actual hardware clock value
2. `delta = now - scd->tick_raw` — since `tick_raw` is 0, delta equals `now`
3. `gtod = scd->tick_gtod + __gtod_offset` — since `tick_gtod` is 0 and `__gtod_offset` is 0, gtod is 0
4. `clock = gtod + delta` = `now`
5. `min_clock = wrap_max(gtod, old_clock)` = `wrap_max(0, scd->clock)`
6. `max_clock = wrap_max(old_clock, gtod + TICK_NSEC)` = `wrap_max(scd->clock, TICK_NSEC)`

On the first call, `scd->clock` is 0, so `max_clock` = `TICK_NSEC`. The computed clock is clamped to `max_clock`, so `scd->clock` is set to `TICK_NSEC` (1,000,000 ns). On subsequent calls, `max_clock` = `wrap_max(TICK_NSEC, TICK_NSEC)` = `TICK_NSEC`, so the returned value remains stuck at `TICK_NSEC` until the scheduler clock infrastructure is properly initialized.

The `sched_clock_local()` function is designed to prevent the clock from jumping more than one tick period at a time (the `max_clock` clamp), which is correct behavior during normal operation but causes the clock to get stuck when operating on uninitialized data.

## Consequence

The observable impact is incorrect timestamps for all early boot messages that use `local_clock()` (or functions that call it, such as `printk` with `CONFIG_PRINTK_TIME` enabled). Instead of monotonically increasing timestamps showing real elapsed time, all messages between the first `local_clock()` call and `sched_clock_init()` show the same timestamp of approximately `[0.001000]` seconds.

As shown in the commit message, on an x86 system with kvm-clock, the buggy kernel produces dmesg output where dozens of boot messages spanning hundreds of milliseconds of real time all show `[0.001000]`:

```
[    0.001000] tsc: ...
[    0.001000] e820: ...
[    0.001000] e820: ...
 ...
[    0.001000] ..TIMER: ...
[    0.001000] clocksource: ...
[    0.378956] Calibrating delay loop ...
```

While this is not a crash or data corruption bug, it significantly hampers the ability to debug early boot issues, measure boot performance, and correlate boot events with real time. The timestamps jump directly from `0.001000` to the correct value (around `0.378956` in the example) once `sched_clock_init()` runs, creating a discontinuity in the timestamp log. Any tooling that relies on monotonic timestamp progression for early boot analysis would be affected.

## Fix Summary

The fix adds the missing `sched_clock_running` guard check to `local_clock()`, exactly mirroring what `sched_clock_cpu()` already does. The added code is:

```c
noinstr u64 local_clock(void)
{
    u64 clock;

    if (static_branch_likely(&__sched_clock_stable))
        return sched_clock() + __sched_clock_offset;

    if (!static_branch_likely(&sched_clock_running))  /* <-- ADDED */
        return sched_clock();                          /* <-- ADDED */

    preempt_disable_notrace();
    clock = sched_clock_local(this_scd());
    preempt_enable_notrace();

    return clock;
}
```

When `sched_clock_init()` has not yet run, the `sched_clock_running` static branch evaluates to false, causing `local_clock()` to return the raw `sched_clock()` value directly. This bypasses `sched_clock_local()` entirely, avoiding the use of uninitialized per-CPU `sched_clock_data` structures. The raw `sched_clock()` value is the best available time source during early boot and provides monotonically increasing, accurate timestamps.

The fix is minimal (3 lines added) and correct because it exactly replicates the existing guard pattern from `sched_clock_cpu()`. The use of `static_branch_likely` ensures there is zero overhead on the fast path once the scheduler clock is initialized, as the branch is patched to be a no-op at runtime. The fix preserves the `noinstr` annotation since `static_branch_likely()` and `sched_clock()` are both safe to call from noinstr contexts.

## Triggering Conditions

The bug is triggered under the following conditions:

1. **Kernel version**: Linux v6.3-rc1 through v6.3 (commits 776f22913b8e to f31dcb152a3d0816e2f1deab4e64572336da197d~1).
2. **Configuration**: `CONFIG_HAVE_UNSTABLE_SCHED_CLOCK=y` must be set (common on x86). When this config is disabled, `local_clock()` is a trivial inline that returns `sched_clock()` directly, and the bug does not apply.
3. **Clock stability**: The scheduler clock must NOT be marked stable (`__sched_clock_stable` is not set), otherwise `local_clock()` returns `sched_clock() + __sched_clock_offset` without reaching the buggy code path. On most virtualized x86 systems (KVM, etc.), the clock is unstable during early boot.
4. **Timing**: Any call to `local_clock()` before `sched_clock_init()` runs will hit the bug. This is a very early boot window — from the point the kernel starts executing with a functional `sched_clock()` source until the scheduler clock subsystem initializes.
5. **Visibility**: The bug is most easily observed with `CONFIG_PRINTK_TIME=y`, which causes printk to use `local_clock()` for timestamps. Without printk timestamps, the bug still occurs (wrong return values from `local_clock()`) but is harder to notice.

The bug is 100% reproducible on any affected kernel version meeting the above conditions — it is not a race condition or timing-sensitive in any way. Every single call to `local_clock()` during the early boot window will return the wrong value (stuck at `TICK_NSEC`).

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. The reasons are:

1. **WHY it cannot be reproduced**: The bug occurs exclusively during very early kernel boot, before `sched_clock_init()` has been called. kSTEP operates as a loadable kernel module (`kmod/`) that is inserted into a running kernel after it has fully booted. By the time any kSTEP driver executes, `sched_clock_init()` has already run and the `sched_clock_running` static key has been enabled. This means the buggy code path (skipping the `sched_clock_running` check and falling through to `sched_clock_local()` with uninitialized data) is impossible to reach from a kernel module context. The `sched_clock_running` static branch, once enabled, cannot be disabled — there is no `sched_clock_deinit()` or similar function.

2. **WHAT would need to be added to kSTEP**: Reproducing this bug would require the ability to execute code during very early kernel boot, before the scheduler clock subsystem is initialized. This is fundamentally incompatible with kSTEP's architecture as a loadable kernel module. One would need either:
   - A mechanism to inject code into the kernel's early init sequence (e.g., a custom init call that runs before `sched_clock_init()`), which would require building the test code directly into the kernel image rather than as a module.
   - A way to reset the scheduler clock subsystem state from a running kernel (disable `sched_clock_running`, zero out per-CPU `sched_clock_data`), which would be extremely dangerous and could destabilize the entire kernel.
   Neither of these approaches is feasible or safe as a minor extension to kSTEP.

3. **Not a version or sched_ext issue**: The bug affects kernels v6.3-rc1 through v6.3, which are within kSTEP's supported version range (v5.15+). The bug is not related to sched_ext.

4. **Alternative reproduction methods**: The bug is trivially reproducible outside kSTEP by simply booting a v6.3 kernel (without the fix) on any x86 system (physical or virtual) with `CONFIG_HAVE_UNSTABLE_SCHED_CLOCK=y` and `CONFIG_PRINTK_TIME=y`. Comparing the dmesg timestamps against a fixed kernel immediately reveals the issue — hundreds of early boot messages will all show `[0.001000]` instead of distinct, increasing timestamps. Alternatively, one could add an `early_initcall` function that calls `local_clock()` and `sched_clock()` and compares the results, printing a diagnostic message if they diverge significantly. A simple QEMU/KVM boot test with `dmesg | grep '0.001000'` would serve as an automated check.
