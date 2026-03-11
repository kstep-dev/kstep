# sched/fair: Fix kernel build warning in test_idle_cores() for !SMT NUMA

- **Commit:** 76c389ab2b5e300698eab87f9d4b7916f14117ba
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** fair (NUMA scheduling)

## Bug Description

When building the kernel with CONFIG_SCHED_SMT disabled (e.g., arm64 defconfig), the compiler emits a warning: "test_idle_cores declared 'static' but never defined". This warning is triggered because a forward declaration of `test_idle_cores()` exists unconditionally, but the function itself is only defined inside `#ifdef CONFIG_SCHED_SMT` guards, making it unused when SMT support is disabled. This prevents clean kernel builds and can cause build failures in strict warning-as-error configurations.

## Root Cause

The forward declaration of `test_idle_cores()` was placed outside any conditional compilation guards, while its actual definition and all call sites were wrapped in `#ifdef CONFIG_SCHED_SMT`. When CONFIG_SCHED_SMT is not set, the declaration exists but is never used, and there is no definition, resulting in an unused function warning from the compiler.

## Fix Summary

The fix moves the forward declaration of `test_idle_cores()` inside the `#ifdef CONFIG_SCHED_SMT` guard so it is only declared when the function is actually used. Additionally, it adds an `#else` section with a stub inline implementation of `numa_idle_core()` for non-SMT builds, ensuring proper code structure in both compilation scenarios.

## Triggering Conditions

This is a compile-time warning issue, not a runtime scheduler bug. The conditions to trigger this warning are:
- Kernel build configuration with `CONFIG_SCHED_SMT=n` (SMT support disabled)
- Common on architectures like arm64 with defconfig
- Compiler warning level includes unused function detection (`-Wunused-function`)
- Build systems configured with warnings-as-errors will fail compilation
- The forward declaration of `test_idle_cores()` exists outside `#ifdef CONFIG_SCHED_SMT` guards
- The actual function definition is only available when `CONFIG_SCHED_SMT=y`
- No runtime scheduler behavior is affected - this is purely a compilation issue

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced using kSTEP since it's a compilation warning, not a runtime scheduler issue. To verify the fix:
- **Compilation test**: Build kernel with `CONFIG_SCHED_SMT=n` and observe compiler warnings
- **No kSTEP driver needed**: The issue manifests at compile time, not during scheduler execution
- **Alternative verification**: Check that `test_idle_cores()` forward declaration is properly guarded
- **Verification approach**: Examine kernel/sched/fair.c source code to confirm declarations match definitions
- **No runtime behavior**: The function is never called when CONFIG_SCHED_SMT is disabled
- **Note**: This fix ensures clean builds on non-SMT configurations without affecting scheduler logic
