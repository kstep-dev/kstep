# sched_ext: Fix possible deadlock in the deferred_irq_workfn()

- **Commit:** a257e974210320ede524f340ffe16bf4bf0dda1e
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

On PREEMPT_RT=y kernels, `deferred_irq_workfn()` executes in the per-cpu irq_work/* task context without disabling interrupts. When the function's rq (obtained via `container_of()`) is the current CPU's runqueue, a deadlock scenario occurs: the task context acquires `rq->__lock`, but if an interrupt fires and also attempts to acquire `rq->__lock`, the system deadlocks due to lock recursion on an non-recursive lock.

## Root Cause

The `init_irq_work()` function initializes the irq_work to run as a regular soft-irq work, which in PREEMPT_RT kernels executes in task context without disabling interrupts. This allows an interrupt to fire while the same lock is held, causing a self-deadlock when both the task context and interrupt context try to acquire `rq->__lock`.

## Fix Summary

The fix replaces `init_irq_work()` with `IRQ_WORK_INIT_HARD()` for `rq->scx.deferred_irq_work` initialization. This ensures `deferred_irq_workfn()` always executes in hard-irq context where interrupts are disabled, preventing the deadlock scenario where an interrupt fires while the same lock is held.

## Triggering Conditions

The deadlock occurs on PREEMPT_RT kernels when:
- A sched_ext BPF scheduler is loaded and `deferred_irq_workfn()` is scheduled to run
- The deferred IRQ work executes in task context (per-cpu irq_work/* thread) on the same CPU whose runqueue is being accessed
- The function acquires `rq->__lock` via `container_of()` from the deferred_irq_work structure
- An interrupt fires while the task context holds `rq->__lock` and the interrupt handler also tries to acquire the same `rq->__lock`
- Since PREEMPT_RT runs IRQ work in task context with interrupts enabled (not disabled), this creates a classic AA deadlock scenario
- The bug is specific to sched_ext subsystem and requires BPF scheduler operations that trigger deferred IRQ work

## Reproduce Strategy (kSTEP)

This bug is difficult to reproduce reliably with kSTEP as it requires PREEMPT_RT kernel configuration and precise interrupt timing. A theoretical approach:
- **CPUs needed**: At least 2 (CPU 0 reserved for driver)  
- **Setup**: Load a minimal sched_ext BPF scheduler that triggers deferred IRQ work via `kstep_sysctl_write()` to enable sched_ext
- **Trigger sequence**: Use `kstep_task_create()` and `kstep_task_wakeup()` to create scheduler activity that would queue deferred IRQ work
- **Observation**: Use `on_tick_begin()` callback to monitor `rq->scx.deferred_irq_work` state and detect when work is queued
- **Detection**: Since actual deadlock would hang the system, monitor for warning messages or unusual lock contention patterns
- **Note**: Reproduction requires PREEMPT_RT=y kernel build and may need additional kernel debug options to observe the deadlock condition without complete system hang
