# preempt/dynamic: Fix setup_preempt_mode() return value

- **Commit:** 9ed20bafc85806ca6c97c9128cec46c3ef80ae86
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The `setup_preempt_mode()` function, which is a `__setup()` callback invoked during kernel boot when processing the `preempt=` parameter, was returning inverted success/failure codes. It returned 0 (failure) on successful parameter parsing and 1 (success) on error. This causes the kernel boot parameter handler to incorrectly interpret valid `preempt=` parameters as failures and invalid ones as successes, breaking the dynamic preemption mode initialization.

## Root Cause

The developer mistakenly inverted the return value convention for `__setup()` callbacks. The kernel's `__setup()` macro expects callbacks to return 1 on success and 0 on failure, but the code had it backwards: returning 1 on error (invalid mode) and 0 on success (valid mode parsed and applied).

## Fix Summary

The fix corrects the return values: return 0 when an unsupported preemption mode is provided, and return 1 when the mode is successfully parsed and applied. This aligns with the `__setup()` callback convention and ensures the kernel correctly recognizes valid dynamic preemption boot parameters.

## Triggering Conditions

This bug manifests during kernel boot parameter processing when CONFIG_PREEMPT_DYNAMIC is enabled. The issue occurs specifically in the `setup_preempt_mode()` function which handles the `preempt=` boot parameter. Two conditions trigger different aspects:

1. **Invalid parameter case**: Passing an unsupported preemption mode (e.g., `preempt=invalid`) incorrectly returns 1 (success), causing the kernel to believe the parameter was handled successfully when it should fail.

2. **Valid parameter case**: Passing a valid mode like `preempt=none`, `preempt=voluntary`, or `preempt=full` incorrectly returns 0 (failure), making the kernel think parameter parsing failed when the mode was actually applied correctly.

The inversion only affects boot parameter validation feedback, not the actual preemption mode functionality.

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced through runtime kSTEP execution since it occurs during kernel boot parameter processing. However, the bug can be validated by examining the kernel's boot parameter handling behavior:

1. **Setup**: Use a kernel build with CONFIG_PREEMPT_DYNAMIC enabled and the buggy code (pre-fix)
2. **Test invalid parameter**: Boot with `preempt=invalid` - the kernel should report parameter failure but incorrectly reports success
3. **Test valid parameter**: Boot with `preempt=none` - the kernel should report success but incorrectly reports failure
4. **Validation**: Check dmesg for "Dynamic Preempt: unsupported mode" warnings and parameter processing return codes

Since this is a boot-time only bug, kSTEP drivers would need to verify the preemption mode was set correctly via `preempt_dynamic_mode` global variable rather than reproducing the parameter parsing itself.
