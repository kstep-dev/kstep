# sched_ext: Fix unsafe locking in the scx_dump_state()

- **Commit:** 5f02151c411dda46efcc5dc57b0845efcdcfc26d
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

On CONFIG_PREEMPT_RT=y kernels, the `dump_lock` in `scx_dump_state()` becomes a sleepable spinlock that doesn't disable interrupts. This causes a lockdep splat indicating an unsafe locking scenario where the rq->__lock is acquired with interrupts enabled in the normal code path, but also acquired from an interrupt context (via irq_work), creating a potential deadlock situation.

## Root Cause

The original code used `rq_lock()` and `rq_unlock()` which don't handle the interrupt state properly on PREEMPT_RT kernels. When `scx_dump_state()` is called from an IRQ work handler (`scx_ops_error_irq_workfn`), it acquires the rq lock without disabling interrupts. If an interrupt fires while the lock is held, it will try to re-acquire the same lock, causing a deadlock.

## Fix Summary

Replace `rq_lock()`/`rq_unlock()` with `rq_lock_irqsave()`/`rq_unlock_irqrestore()` in the CPU state dumping loop. This ensures interrupts are properly saved and restored around the critical section, preventing the unsafe interrupt scenario on PREEMPT_RT systems.

## Triggering Conditions

- **Kernel configuration**: CONFIG_PREEMPT_RT=y must be enabled (makes spinlocks sleepable)
- **Scheduler subsystem**: sched_ext subsystem with active BPF scheduler operations
- **IRQ work context**: `scx_dump_state()` called from `scx_ops_error_irq_workfn` in IRQ work handler
- **Lock contention**: Nested rq->__lock acquisition where outer lock is held with interrupts enabled, inner acquisition happens during interrupt
- **Race condition**: Timer interrupt (sched_tick) fires while rq lock held in dump context, both try to acquire same rq lock
- **CPU state**: Any CPU where sched_ext operations are active and error dump is triggered

## Reproduce Strategy (kSTEP)

Note: This bug requires CONFIG_PREEMPT_RT=y and sched_ext support, which may not be available in standard kSTEP builds. This strategy assumes theoretical extensions to support sched_ext operations.

- **CPU requirements**: At least 2 CPUs (CPU 0 reserved, CPU 1+ for testing)
- **Setup**: Create sched_ext BPF scheduler program that can trigger error conditions, enable CONFIG_PREEMPT_RT kernel
- **IRQ simulation**: Use `kstep_tick_repeat()` to generate timer interrupts while holding dump_lock in error path
- **Error injection**: Trigger sched_ext error via malformed BPF program to invoke `scx_ops_error_irq_workfn`
- **Lock detection**: Use lockdep instrumentation callbacks or kernel tracing to detect rq lock acquisition patterns
- **Verification**: Monitor for lockdep warnings about "inconsistent {IN-HARDIRQ-W} -> {HARDIRQ-ON-W} usage" or actual deadlock
- **Alternative**: Direct kernel module to simulate the locking pattern by calling rq_lock in normal context, then trigger interrupt that also tries rq_lock
