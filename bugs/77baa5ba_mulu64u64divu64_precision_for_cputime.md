# sched/cputime: Fix mul_u64_u64_div_u64() precision for cputime

- **Commit:** 77baa5bafcbe1b2a15ef9c37232c21279c95481c
- **Affected file(s):** kernel/sched/cputime.c
- **Subsystem:** cputime

## Bug Description

In the `cputime_adjust()` function, the calculation of `stime` via `mul_u64_u64_div_u64()` can lose precision on some architectures (ARM64), causing `stime` to exceed `rtime`. When `utime` is subsequently computed as `rtime - stime`, an unsigned integer underflow occurs, resulting in a huge incorrect value appearing in the 14th field (utime) of /proc/xx/stat.

## Root Cause

The `mul_u64_u64_div_u64()` function does not guarantee perfect precision in the computation of `stime * rtime / (stime + utime)`. On architectures like ARM64 with `TICK_CPU_ACCOUNTING=y`, the result can violate the mathematical constraint that the computation should produce a value less than or equal to `rtime`. When this constraint is violated and `stime > rtime`, the subsequent subtraction `utime = rtime - stime` wraps around due to unsigned integer arithmetic.

## Fix Summary

The fix adds a post-computation check after `mul_u64_u64_div_u64()` to enforce the constraint that `stime <= rtime`. If `stime` exceeds `rtime` due to precision loss, it is capped to `rtime`, preventing the unsigned underflow and ensuring correct utime values in /proc/xx/stat.

## Triggering Conditions

The bug occurs in the `cputime_adjust()` function within the cputime subsystem when:
- Tasks execute primarily in kernel mode (high stime relative to utime)
- Sufficient runtime accumulates to trigger precision loss in `mul_u64_u64_div_u64(stime, rtime, stime + utime)`
- The computed stime value exceeds rtime due to rounding errors on specific architectures
- ARM64 architecture with `TICK_CPU_ACCOUNTING=y` and `CONFIG_VIRT_CPU_ACCOUNTING_NATIVE` unset
- The bug manifests when `cputime_adjust()` is called during task stat reporting or accounting updates
- Requires scenarios where `stime * rtime / (stime + utime) > rtime` due to floating-point precision issues

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create kernel-heavy tasks via `kstep_task_create()` and induce system call workload:
- In `setup()`: Create multiple tasks with `kstep_task_create()` and configure them for intensive kernel operations
- In `run()`: Use `kstep_task_wakeup()` to start tasks, then `kstep_tick_repeat(1000+)` to accumulate significant runtime
- Force kernel-mode execution through repeated system calls or I/O operations via task manipulation
- Monitor cputime values using task accounting callbacks (`on_tick_end`) to track stime/utime/rtime progression  
- Check for condition where computed stime > rtime before the fix's constraint enforcement
- Validate bug trigger by parsing /proc/pid/stat field 14 (utime) for overflow values (near UINT64_MAX)
- Compare behavior on buggy vs fixed kernels to confirm underflow elimination
