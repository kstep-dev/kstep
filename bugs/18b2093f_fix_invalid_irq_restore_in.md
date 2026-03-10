# sched_ext: Fix invalid irq restore in scx_ops_bypass()

- **Commit:** 18b2093f4598d8ee67a8153badc93f0fa7686b8a
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext - extensible scheduler)

## Bug Description

The `scx_ops_bypass()` function performs nested interrupt-safe locking with an outer irqsave/restore wrapper and inner per-CPU queue locks. When the code path encounters a disabled scheduler state, it incorrectly uses `rq_unlock_irqrestore()` instead of `rq_unlock()`, causing the inner unlock to prematurely restore interrupt flags. This triggers a kernel warning "raw_local_irq_restore() called with IRQs enabled" indicating that the interrupt flag restoration violates expected nesting semantics.

## Root Cause

Commit 0e7ffff1b811 added outer irqsave/restore locking to `scx_ops_bypass()` but missed converting an inner `rq_unlock_irqrestore()` call to `rq_unlock()`. When nesting interrupt-safe locks, the inner lock release must not restore interrupt flags (which is handled by the outer scope); the stray `_irqrestore()` suffix causes the inner unlock to attempt IRQ restoration, violating the locking hierarchy and producing a consistency warning.

## Fix Summary

The fix changes one line in the early-exit path: replace `rq_unlock_irqrestore(rq, &rf)` with `rq_unlock(rq, &rf)` so that interrupt flag restoration is deferred to the outer irqsave/restore pair, ensuring correct nesting semantics and eliminating the spurious warning.

## Triggering Conditions

The bug is triggered when `scx_ops_bypass()` is called with outer irqsave/restore locking during sched_ext scheduler transitions. Specifically:
- sched_ext subsystem must be compiled in and a BPF scheduler program is being registered
- The `scx_ops_bypass()` function executes its per-CPU loop with `rq_lock(rq, &rf)` 
- The `scx_enabled()` check returns false (scheduler not yet fully enabled during transition)
- Early exit path executes `rq_unlock_irqrestore()` instead of `rq_unlock()`
- This premature IRQ flag restoration within nested irqsave/restore triggers the warning

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce with kSTEP due to sched_ext dependencies. A theoretical approach would be:
- **CPUs needed:** 2+ (CPU 0 reserved for driver)
- **Setup:** Kernel must have CONFIG_SCHED_CLASS_EXT=y and BPF support
- **Limitation:** Current kSTEP lacks sched_ext API functions for BPF scheduler registration
- **Alternative approach:** Direct kernel module that calls `scx_ops_bypass()` during scheduler state transitions
- **Detection:** Monitor for "raw_local_irq_restore() called with IRQs enabled" warning in kernel logs
- **Validation:** Run on fixed kernel to confirm warning disappears

Note: Full reproduction requires extending kSTEP with sched_ext support or using a dedicated kernel module that manipulates sched_ext state transitions directly.
