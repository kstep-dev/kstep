# Core: cputime_adjust unsigned overflow from mul_u64_u64_div_u64() approximation

**Commit:** `77baa5bafcbe1b2a15ef9c37232c21279c95481c`
**Affected files:** `kernel/sched/cputime.c`
**Fixed in:** v6.11-rc2
**Buggy since:** v5.9-rc1 (commit `3dc167ba5729` "sched/cputime: Improve cputime_adjust()")

## Bug Description

The `cputime_adjust()` function in `kernel/sched/cputime.c` is responsible for splitting a task's total runtime (`sum_exec_runtime`, called `rtime`) into user time (`utime`) and system time (`stime`) based on the ratio of tick-based `utime` and `stime` samples. This function is called when userspace reads `/proc/<pid>/stat` to report the 14th (utime) and 15th (stime) fields. The function must guarantee that `utime + stime == rtime` and that both values are monotonically non-decreasing.

The core of the computation is `stime = mul_u64_u64_div_u64(stime, rtime, stime + utime)`, which mathematically computes `stime * rtime / (stime + utime)`. Since `stime <= stime + utime`, the mathematical result should always satisfy `stime_result <= rtime`. The subsequent line computes `utime = rtime - stime`, which relies on `stime <= rtime` to avoid unsigned underflow.

On architectures that lack native 128-bit multiply/divide support (such as ARM64 at the time of the bug), the generic fallback implementation of `mul_u64_u64_div_u64()` uses an approximation algorithm that drops precision by shifting operands right. This approximation can produce a result slightly *larger* than the mathematically correct value, violating the invariant `stime <= rtime`. When this happens, `utime = rtime - stime` wraps around to a massive unsigned value near `UINT64_MAX`.

The bug was observed in production on ARM64 with `TICK_CPU_ACCOUNTING=y` and `CONFIG_VIRT_CPU_ACCOUNTING_NATIVE` not set. The task was running primarily in kernel mode, resulting in a very large `stime` relative to `utime`. The specific values reported were: `stime = 175136586720000`, `rtime = 135989749728000`, `utime = 1416780000`. After `mul_u64_u64_div_u64()`, `stime` became `135989949653530`, which is `199925530` nanoseconds larger than `rtime`. This caused `utime` to wrap to `18446744073709518790`.

## Root Cause

The root cause is in the generic (non-x86) implementation of `mul_u64_u64_div_u64()` (which maps to `mul_u64_add_u64_div_u64()` via a macro). On architectures without native 128-bit arithmetic, the generic version must emulate the 128-bit multiply-then-divide using 64-bit operations. The old generic implementation worked as follows:

1. It first checks if `a * b` would overflow 64 bits using `ilog2(a) + ilog2(b) > 62`.
2. If overflow is possible, it ensures `a <= b`, then computes `b / c` and `b % c` to reduce the problem.
3. If the reduced multiplication `a * (b % c)` would still overflow, it drops precision by right-shifting both `b` and `c` by a computed shift amount: `shift = ilog2(a) + ilog2(b) - 62`.

The precision loss occurs in step 3. When `b` and `c` are right-shifted, the low bits are discarded. The division `(a * b_shifted) / c_shifted` can produce a result that differs from the true value of `(a * b) / c`. Critically, this approximation can produce a value *larger* than the true quotient because the rounding of `c` (the divisor) downward makes the quotient larger.

In the `cputime_adjust()` case, the computation is `stime_new = stime * rtime / (stime + utime)`. When `utime` is very small relative to `stime` (as in the reported case: `utime = 1416780000` vs `stime = 175136586720000`, a ratio of about 1:123600), the denominator `stime + utime` is barely larger than the numerator's first factor `stime`. The mathematical result should be close to but slightly less than `rtime`. The approximation error pushes the result just past `rtime`.

Specifically, with the reported values: the exact result of `175136586720000 * 135989749728000 / 175138003500000` is approximately `135988649638049.6`, which is less than `rtime = 135989749728000`. But the generic `mul_u64_u64_div_u64()` returned `135989949653530`, which exceeds `rtime` by `199925530`. On x86_64, which has a native implementation using the `mulq` and `divq` instructions for exact 128-bit arithmetic, this computation returns the correct value and the bug never manifests.

## Consequence

The immediate consequence is that `utime` reported in `/proc/<pid>/stat` wraps around to a near-maximum 64-bit unsigned value (e.g., `18446744073709518790` nanoseconds). This is a catastrophically wrong value — it represents approximately 584 years instead of the correct sub-second amount.

Any userspace tool that reads and parses `/proc/<pid>/stat` to compute CPU time breakdowns (such as `top`, `htop`, `ps`, monitoring agents, container resource accounting, and billing systems) would see wildly incorrect utime values. Since the value wraps to near `UINT64_MAX`, it would appear that the process has consumed an impossibly large amount of user CPU time. Tools that compute incremental CPU usage by subtracting consecutive readings would see enormous jumps followed by apparent negative usage (when the next reading is correct), causing erratic displays and incorrect resource accounting.

The bug is not a crash or security vulnerability, but a data corruption issue affecting the accuracy of CPU time reporting. It is particularly insidious because it only manifests under specific conditions (architecture + workload pattern + large accumulated cputime values) and can go unnoticed for long periods until the values become large enough for the approximation error to push `stime` past `rtime`.

## Fix Summary

The fix adds a simple bounds check after the `mul_u64_u64_div_u64()` call in `cputime_adjust()`:

```c
stime = mul_u64_u64_div_u64(stime, rtime, stime + utime);
/*
 * Because mul_u64_u64_div_u64() can approximate on some
 * architectures; enforce the constraint that: a*b/(b+c) <= a.
 */
if (unlikely(stime > rtime))
    stime = rtime;
```

This enforces the mathematical invariant that `stime * rtime / (stime + utime) <= rtime` regardless of any approximation error in the underlying multiplication/division. If the approximation overshoots, `stime` is clamped to `rtime`, which means `utime = rtime - stime = 0`. While this is not the exact correct ratio, it's the best safe bound and preserves the monotonicity guarantees of the rest of the function.

The fix is correct and complete because: (1) it preserves the existing monotonicity logic that follows (the `prev->stime` and `prev->utime` checks still work correctly), (2) clamping `stime` to `rtime` is a conservative but safe approximation that only triggers on rare precision errors, and (3) it addresses all architectures uniformly without needing per-architecture fixes. Peter Zijlstra also suggested creating a native ARM64 implementation of `mul_u64_u64_div_u64()` for better precision, but that is a separate enhancement. The generic implementation was later rewritten with a proper long-division algorithm, which likely eliminates this precision issue entirely.

## Triggering Conditions

The bug requires all of the following conditions simultaneously:

- **Architecture without native 128-bit multiply/divide**: The architecture must use the generic fallback implementation of `mul_u64_u64_div_u64()`. This means specifically NOT x86_64, which has a custom implementation using `mulq`/`divq` instructions. ARM64 is the primary affected architecture, but any architecture using the generic `lib/math/div64.c` fallback is susceptible.

- **TICK_CPU_ACCOUNTING=y and CONFIG_VIRT_CPU_ACCOUNTING_NATIVE not set**: The `cputime_adjust()` function is only compiled and used when tick-based CPU accounting is active. If `VIRT_CPU_ACCOUNTING_NATIVE` is set, the kernel uses precise per-instruction accounting and `cputime_adjust()` is not called.

- **Task running predominantly in kernel mode**: The `stime` value must be very large relative to `utime`. The precision error is more likely to produce `stime > rtime` when `utime` is tiny compared to `stime`, because the mathematical result `stime * rtime / (stime + utime)` is very close to `rtime` in that case. In the reported case, the ratio was approximately 123600:1 (stime:utime).

- **Large accumulated cputime values**: The values must be large enough that `ilog2(stime) + ilog2(rtime) > 62`, triggering the approximation code path in the generic `mul_u64_u64_div_u64()`. The reported values had `stime ≈ 1.75e14` and `rtime ≈ 1.36e14`, both around 47 bits, easily exceeding the 62-bit threshold.

- **Specific value alignment**: Not all large values trigger the bug; the precision loss from right-shifting must push the approximated result past `rtime`. This depends on the exact bit patterns of the operands and how the shift truncation affects the quotient. The bug is not deterministic for arbitrary large values but is reliably triggered for specific value combinations.

## Reproduce Strategy (kSTEP)

### Why this bug cannot be reproduced with kSTEP

1. **Architecture mismatch**: kSTEP runs inside QEMU, and the current host and build environment is x86_64. On x86_64, the kernel uses a custom `mul_u64_add_u64_div_u64()` implementation (in `arch/x86/include/asm/div64.h`) that performs exact 128-bit arithmetic using the native `mulq` and `divq` x86 instructions. This means the computation `stime * rtime / (stime + utime)` will always produce the mathematically correct result (truncated to u64), and the condition `stime > rtime` can never occur. The bug is fundamentally impossible to trigger on x86_64.

2. **No way to force generic fallback**: There is no kernel configuration option or runtime mechanism to force the kernel to use the generic `mul_u64_u64_div_u64()` implementation when a native architecture-specific version is available. The selection is made at compile time via `#define mul_u64_add_u64_div_u64` in the architecture-specific header, which prevents the generic version from even being compiled.

3. **Observation difficulty**: Even if the bug could be triggered, observing its consequence (incorrect utime/stime values) would require reading `/proc/<pid>/stat` from userspace, which is not directly possible from a kSTEP kernel module. While kSTEP can access internal task structures, the `cputime_adjust()` function itself would need to be called with specific value combinations, and on x86_64 it would never produce the erroneous result.

### What would need to change in kSTEP

To reproduce this bug, kSTEP would need one of the following fundamental changes:

- **ARM64/aarch64 QEMU support with generic mul_u64_u64_div_u64()**: If kSTEP were built and run on an aarch64 QEMU instance that uses the generic fallback, the bug could potentially be triggered. However, even this is uncertain because the generic implementation has been rewritten since the bug was fixed (the current version uses a long-division algorithm that is more precise).

- **Override mul_u64_u64_div_u64()**: If kSTEP could patch or override the `mul_u64_u64_div_u64()` function to use the old generic fallback implementation, then a driver could set up task cputime values and trigger `cputime_adjust()`. This would require either binary patching or a separate compilation of the old generic code, neither of which is a minor extension.

### Alternative reproduction methods

The bug can be reproduced outside kSTEP by:

1. **Direct computation test**: Run the old generic `mul_u64_u64_div_u64()` algorithm (as provided by Oleg Nesterov in the email thread) in userspace with the reported values: `mul_u64_u64_div_u64(175136586720000, 135989749728000, 175138003500000)`. This directly demonstrates the precision error producing `135989949653530 > 135989749728000`.

2. **ARM64 kernel test**: On a real or emulated ARM64 system with `TICK_CPU_ACCOUNTING=y` and `CONFIG_VIRT_CPU_ACCOUNTING_NATIVE` not set, run a workload that spends most of its time in kernel mode (e.g., heavy syscall workload or spinlock contention) for many hours until the accumulated cputime values are large enough, then read `/proc/<pid>/stat` and check for utime values near `UINT64_MAX`.

3. **Kernel unit test**: Write a test that directly calls `cputime_adjust()` with crafted `task_cputime` and `prev_cputime` structures containing the known-bad values, running on an ARM64 kernel. Check if the resulting utime wraps.
