# sched/core: Prevent wakeup of ksoftirqd during idle load balance

- **Commit:** e932c4ab38f072ce5894b2851fea8bc5754bb8e5
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When the scheduler raises SCHED_SOFTIRQ from the idle load balancing SMP function handler (nohz_csd_func), it unnecessarily wakes up ksoftirqd. The HARD-IRQ flag is not set in this context, causing raise_softirq_irqoff() to treat soft interrupts as pending and wake the ksoftirqd thread. However, soft interrupts are guaranteed to be handled before ksoftirqd gets on the CPU, making this wakeup wasteful and causing unnecessary context switches.

## Root Cause

The function nohz_csd_func() is an SMP function handler that is always invoked in an interrupt context on the target CPU. However, because the HARD-IRQ flag is not set in this specific context, the generic raise_softirq_irqoff() function incorrectly determines that it needs to wake ksoftirqd to service pending soft interrupts. This results in an unnecessary context switch even though the softirq will be handled in the normal interrupt return path.

## Fix Summary

Replace raise_softirq_irqoff() with __raise_softirq_irqoff() in nohz_csd_func(). The lower-level __raise_softirq_irqoff() variant does not wake ksoftirqd and is appropriate here because the SMP function is guaranteed to be invoked in interrupt context where soft interrupts are always processed.

## Triggering Conditions

The bug occurs during idle load balancing when nohz_csd_func() is invoked as an SMP function handler on an idle CPU. Key conditions:
- Target CPU must be in idle state (running swapper/idle task)
- NOHZ idle load balancing must be triggered (via kick_ilb() or similar)
- SMP function call executed via flush_smp_call_function_queue() from idle context
- HARD-IRQ flag is not set in this specific SMP call context
- raise_softirq_irqoff() incorrectly determines ksoftirqd wakeup is needed
- Results in unnecessary context switch: idle → ksoftirqd → idle
- Softirq processing occurs twice: once immediately, then redundantly via ksoftirqd

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver, CPU 1+ for targets). Create load imbalance to trigger idle load balancing:
- **Setup**: Configure topology with multiple CPUs using kstep_topo_init() and kstep_topo_apply()
- **Create imbalance**: Use kstep_task_create() and kstep_task_pin() to place tasks unevenly (e.g., all tasks on CPU 1, leaving CPU 2+ idle)
- **Force idle balancing**: Call kstep_tick_repeat() to advance time and trigger rebalance_domains() on idle CPUs
- **Monitor events**: Use on_sched_softirq_begin/end callbacks to detect SCHED_SOFTIRQ handling
- **Detection**: Check for ksoftirqd wakeup events using task state monitoring or trace log analysis
- **Verification**: Compare ksoftirqd wakeup frequency between buggy and fixed kernels during identical load patterns
