# Core: sched_feat() Toggle Silently Ignored Without CONFIG_JUMP_LABEL

**Commit:** `a73f863af4ce9730795eab7097fb2102e6854365`
**Affected files:** kernel/sched/core.c, kernel/sched/sched.h
**Fixed in:** v5.10-rc1
**Buggy since:** v4.15-rc1 (commit `765cc3a4b224e`)

## Bug Description

The Linux scheduler exposes a set of runtime-tunable feature flags through the debugfs file `/sys/kernel/debug/sched_features`. These flags control various scheduler behaviors such as `GENTLE_FAIR_SLEEPERS`, `PLACE_LAG`, `START_DEBIT`, `NEXT_BUDDY`, `LAST_BUDDY`, `NONTASK_CAPACITY`, `TTWU_QUEUE`, `SIS_UTIL`, and others. On `CONFIG_SCHED_DEBUG=y` kernels, administrators can toggle these features at runtime by writing to the debugfs file (e.g., `echo NO_GENTLE_FAIR_SLEEPERS > /sys/kernel/debug/sched_features`).

When `CONFIG_JUMP_LABEL` is available, these toggles are implemented using static keys, which patch the instruction stream in-place for zero-overhead branching. When `CONFIG_JUMP_LABEL` is not available, the toggles should fall back to checking a shared global variable `sysctl_sched_features` at runtime via bitmask operations.

Commit `765cc3a4b224e` ("sched/core: Optimize sched_feat() for !CONFIG_SCHED_DEBUG builds") introduced a compile-time optimization that makes `sysctl_sched_features` a per-translation-unit static constant when `CONFIG_SCHED_DEBUG` is disabled, allowing the compiler to propagate constants and eliminate dead code for disabled features. However, this commit structured the `#ifdef` guards incorrectly: the condition `#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_JUMP_LABEL)` was used to gate the shared extern declaration of `sysctl_sched_features` and its corresponding definition in `core.c`. This meant that when `CONFIG_SCHED_DEBUG=y` but `CONFIG_JUMP_LABEL` was not set, the code would fall through to the `!CONFIG_SCHED_DEBUG` branch, which defines a per-file static copy of `sysctl_sched_features`.

The result was that in `CONFIG_SCHED_DEBUG=y && !CONFIG_JUMP_LABEL` configurations, writing to `/sys/kernel/debug/sched_features` would update the copy in `core.c` and change what the file reports when read back, but all other scheduler source files (`fair.c`, `rt.c`, `deadline.c`, etc.) would continue using their own independent static copies that were initialized at compile time and never updated. The scheduler's behavior was completely unaffected by the debugfs toggle — a silent, deceptive failure.

## Root Cause

The root cause lies in the preprocessor conditional structure in `kernel/sched/sched.h` and the corresponding guard in `kernel/sched/core.c`. Prior to the fix, the code had two branches:

1. `#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_JUMP_LABEL)`: This branch declared `extern const_debug unsigned int sysctl_sched_features`, defined the `sched_feat()` macro using static keys, and expected `core.c` to provide the actual definition.

2. `#else`: This was intended for `!CONFIG_SCHED_DEBUG` but actually caught **all** other combinations, including `CONFIG_SCHED_DEBUG=y && !CONFIG_JUMP_LABEL`. This branch defined a per-translation-unit `static const_debug __maybe_unused unsigned int sysctl_sched_features` initialized from the `features.h` defaults, and defined `sched_feat(x)` as `!!(sysctl_sched_features & (1UL << __SCHED_FEAT_##x))`.

In `core.c`, the definition of the shared `sysctl_sched_features` variable was also guarded by `#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_JUMP_LABEL)`. When `CONFIG_JUMP_LABEL` was not defined, `core.c` would not provide the shared definition. Instead, both `core.c` and every other `.c` file including `sched.h` would each get their own independent static copy.

The debugfs write handler (`sched_feat_write()` in `kernel/sched/debug.c`) modifies `sysctl_sched_features` — but in this configuration, `debug.c` has its own static copy. When it updates that copy, the change is visible when reading back from debugfs (since `debug.c` reads from its own copy), but `fair.c`, `rt.c`, and every other scheduler file continue to use their own unchanged copies.

Specifically, the problematic `#else` clause in `sched.h` was:
```c
#else /* !(SCHED_DEBUG && CONFIG_JUMP_LABEL) */
/*
 * Each translation unit has its own copy of sysctl_sched_features to allow
 * constants propagation at compile time and compiler optimization based on
 * features default.
 */
#define SCHED_FEAT(name, enabled)	\
	(1UL << __SCHED_FEAT_##name) * enabled |
static const_debug __maybe_unused unsigned int sysctl_sched_features =
#include "features.h"
	0;
#undef SCHED_FEAT

#define sched_feat(x) !!(sysctl_sched_features & (1UL << __SCHED_FEAT_##x))
#endif
```

This per-TU static definition was only correct for `!CONFIG_SCHED_DEBUG` kernels. For `CONFIG_SCHED_DEBUG=y && !CONFIG_JUMP_LABEL`, it was incorrect because the intent was for all translation units to share a single mutable variable so that debugfs writes would propagate everywhere.

## Consequence

The primary consequence is that runtime toggling of scheduler features via `/sys/kernel/debug/sched_features` becomes completely ineffective on kernels built with `CONFIG_SCHED_DEBUG=y` and `!CONFIG_JUMP_LABEL`. The debugfs interface gives the illusion that features are being toggled (reads reflect the changed value), but the actual scheduler code paths in `fair.c`, `rt.c`, `deadline.c`, and other files continue to use the compile-time defaults.

This is a particularly insidious class of bug because there is no crash, no warning, no log message — just silent misbehavior. An administrator or developer who tries to disable a feature like `GENTLE_FAIR_SLEEPERS` to debug a scheduling issue would see the debugfs file confirm the change but observe no difference in behavior, potentially leading them to conclude the feature flag is unrelated to their problem. This can waste significant debugging effort.

The affected configuration (`CONFIG_SCHED_DEBUG=y && !CONFIG_JUMP_LABEL`) is uncommon on modern x86 systems (where `CONFIG_JUMP_LABEL` is typically enabled), but it can appear on architectures that lack jump label support, on deliberately minimal kernel configurations, or when `CONFIG_JUMP_LABEL` is disabled for debugging or bisecting purposes. Embedded ARM systems and some older architectures are more likely to encounter this configuration.

## Fix Summary

The fix restructures the `#ifdef` nesting to correctly handle all three relevant configurations:

1. **`CONFIG_SCHED_DEBUG=y && CONFIG_JUMP_LABEL`**: Uses the extern shared `sysctl_sched_features` and static keys for zero-overhead `sched_feat()` checks. Unchanged from before.

2. **`CONFIG_SCHED_DEBUG=y && !CONFIG_JUMP_LABEL`** (newly added case): Uses the extern shared `sysctl_sched_features` but with a simple bitmask check instead of static keys: `#define sched_feat(x) (sysctl_sched_features & (1UL << __SCHED_FEAT_##x))`. This ensures all translation units reference the same variable defined in `core.c`, so debugfs writes propagate correctly.

3. **`!CONFIG_SCHED_DEBUG`**: Retains the per-TU static constant for compile-time optimization. Unchanged from before.

In `sched.h`, the fix changes the outer guard from `#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_JUMP_LABEL)` to `#ifdef CONFIG_SCHED_DEBUG`, and nests the jump-label-specific code under an inner `#ifdef CONFIG_JUMP_LABEL` / `#else` / `#endif` block within the `CONFIG_SCHED_DEBUG` section. In `core.c`, the guard for the shared `sysctl_sched_features` definition is similarly relaxed from `#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_JUMP_LABEL)` to `#ifdef CONFIG_SCHED_DEBUG`, ensuring the single shared definition exists whenever debug features are enabled, regardless of jump label availability.

## Triggering Conditions

To trigger this bug, the following conditions must all be met:

- **Kernel configuration**: The kernel must be built with `CONFIG_SCHED_DEBUG=y` (enables the debugfs sched_features interface) and `CONFIG_JUMP_LABEL` **not** set (disables static key patching). `CONFIG_SCHED_DEBUG` is typically enabled in distribution debug kernels. `CONFIG_JUMP_LABEL` depends on architecture support (`HAVE_ARCH_JUMP_LABEL`) and may be disabled on architectures that lack it, or explicitly disabled in custom configurations.

- **Runtime action**: An administrator or script must write to `/sys/kernel/debug/sched_features` to toggle a feature flag. For example: `echo NO_GENTLE_FAIR_SLEEPERS > /sys/kernel/debug/sched_features` or `echo NEXT_BUDDY > /sys/kernel/debug/sched_features`.

- **Observation**: After the write, the scheduler behavior that the toggled feature controls must be observed. The bug is that the behavior does **not** change despite the debugfs file reporting the new setting. For example, after disabling `GENTLE_FAIR_SLEEPERS`, newly waking tasks should not receive a vruntime credit, but on the buggy kernel they still do.

There is no race condition or timing requirement — the bug is deterministic for the affected configuration. The probability of encountering it depends entirely on the kernel build configuration. On modern x86_64 systems with standard distribution configs, `CONFIG_JUMP_LABEL` is almost always enabled, making this bug very unlikely to manifest. On embedded or minimal configurations without jump label support, the bug is 100% reproducible.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **KERNEL VERSION TOO OLD**: The fix was merged in commit `a73f863af4ce9730795eab7097fb2102e6854365`, which is part of v5.10-rc1 (the `describe --contains` output confirms it shipped in v5.10-rc1). kSTEP only supports Linux v5.15 and newer. The buggy code (with the incorrect `#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_JUMP_LABEL)` guard) exists only in kernels from v4.15-rc1 (when commit `765cc3a4b224e` was introduced) through v5.9 (the last release before the fix). All kSTEP-supported kernels (v5.15+) already contain the fix, so there is no kernel version available to kSTEP that exhibits the buggy behavior.

2. **Build configuration requirement**: Even if the kernel version were supported, reproducing this bug would require building the kernel with `CONFIG_JUMP_LABEL` disabled. On x86_64 (which kSTEP uses via QEMU), `CONFIG_JUMP_LABEL` is available and typically enabled by default. While it could theoretically be disabled, this is a non-standard configuration that would require custom kernel config modifications.

3. **Debugfs write requirement**: The bug manifests when writing to the debugfs file `/sys/kernel/debug/sched_features`. kSTEP provides `kstep_sysctl_write()` for sysctl files, but sched_features is a debugfs file, not a sysctl. A kernel module could potentially write to the debugfs file using VFS operations (`kernel_write()` or calling the debugfs handler directly), but this would require adding a new helper like `kstep_debugfs_write()` or `kstep_sched_feat_write()`.

4. **Behavioral observation**: To confirm the bug, one would need to observe that a scheduler feature toggle did not take effect. This requires choosing a feature with observable behavioral impact (e.g., `GENTLE_FAIR_SLEEPERS` affecting vruntime placement of waking tasks) and then measuring whether the behavior changes after the toggle. While this is conceptually possible in kSTEP, the combination of version incompatibility makes it moot.

5. **Alternative reproduction methods**: Outside kSTEP, this bug can be reproduced by:
   - Building a v4.15–v5.9 kernel with `CONFIG_SCHED_DEBUG=y` and `CONFIG_JUMP_LABEL=n`
   - Booting the kernel (on real hardware or in a VM)
   - Reading `/sys/kernel/debug/sched_features` to note the defaults
   - Toggling a feature: `echo NO_GENTLE_FAIR_SLEEPERS > /sys/kernel/debug/sched_features`
   - Verifying the debugfs file reflects the change: `cat /sys/kernel/debug/sched_features`
   - Running a workload that exercises the toggled feature and observing no behavioral difference
   - Alternatively, using `gdb` or `crash` to inspect `sysctl_sched_features` in different modules to see they hold different values
   - Or inserting `printk` in `fair.c` to log `sched_feat(GENTLE_FAIR_SLEEPERS)` and confirming it never changes

The fundamental blocker is the kernel version: the fix predates kSTEP's minimum supported version by several releases, making it impossible to check out a buggy kernel within kSTEP's framework.
