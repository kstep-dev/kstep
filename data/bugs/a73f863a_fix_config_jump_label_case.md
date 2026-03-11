# sched/features: Fix !CONFIG_JUMP_LABEL case

- **Commit:** a73f863af4ce9730795eab7097fb2102e6854365
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** Core scheduler

## Bug Description

When CONFIG_SCHED_DEBUG=y but CONFIG_JUMP_LABEL is disabled, echoing changes to /sys/kernel/debug/sched_features reports changes but does not actually affect scheduler behavior. The sysctl interface appears to work—users can write to the feature flags and see the changes reflected—but different translation units maintain separate copies of sysctl_sched_features, so the changes only affect the copy in core.c while other translation units continue using stale values.

## Root Cause

A previous optimization (commit 765cc3a4b224e) made sched features static for !CONFIG_SCHED_DEBUG builds but overlooked the interaction between CONFIG_SCHED_DEBUG=y and !CONFIG_JUMP_LABEL. The ifdef conditions were too restrictive: `#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_JUMP_LABEL)` meant that the single shared sysctl_sched_features was only exported in the CONFIG_JUMP_LABEL case. When CONFIG_JUMP_LABEL was disabled, the code fell through to the static per-unit copy path, creating inconsistent state where the sysctl interface was advertised but ineffective.

## Fix Summary

The fix restructures the ifdef hierarchy so that CONFIG_SCHED_DEBUG=y always exports a shared sysctl_sched_features (in core.c) that all translation units reference, regardless of CONFIG_JUMP_LABEL. The sched_feat() macro is then conditionally defined: using static_key branching if CONFIG_JUMP_LABEL is available, or falling back to reading from the shared sysctl_sched_features variable otherwise. This ensures consistent behavior and allows the sysctl interface to actually control scheduler behavior across all code paths.

## Triggering Conditions

This bug occurs when the kernel is compiled with CONFIG_SCHED_DEBUG=y but CONFIG_JUMP_LABEL=n. Under these configuration conditions, the sched_feat() macro expansion in different translation units (core.c vs. fair.c, rt.c, etc.) reference different copies of sysctl_sched_features. The /sys/kernel/debug/sched_features interface only modifies the copy in core.c, leaving other translation units using stale static copies with default values. The bug manifests when userspace writes to sched_features expecting to change scheduler behavior, but only core.c sees the changes while fair.c, rt.c continue using original values.

## Reproduce Strategy (kSTEP)

This is a configuration bug that requires demonstrating inconsistent sched_feat() behavior across translation units. Use 2+ CPUs. In setup(), create tasks and potentially enable a scheduler feature that affects runtime behavior (e.g., START_DEBIT, GENTLE_FAIR_SLEEPERS). In run(), use kstep_write() to modify /sys/kernel/debug/sched_features to toggle the feature. Then create scenarios where both core.c and fair.c would evaluate sched_feat() differently—such as task wakeups that trigger place_entity() in fair.c. Use on_tick_begin() callback to log feature state from different code paths. Check logs to verify that feature changes from sysctl interface are not consistently applied across all scheduler subsystems, demonstrating the configuration bug.
