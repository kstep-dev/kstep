# sched: Reduce the default slice to avoid tasks getting an extra tick

- **Commit:** 2ae891b826958b60919ea21c727f77bcd6ffcc2c
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

Tasks running on systems with HZ=1000 and 8 or more CPUs receive an extra tick beyond their assigned time slice. When a task's deadline (vruntime + sysctl_sched_base_slice) approaches, the next tick often arrives slightly earlier than expected due to clockevent precision or time accounting overhead. This causes the task to perceive the deadline as not yet reached, running an additional 1ms (one full tick) beyond its intended slice allocation.

## Root Cause

The default base slice of 0.75 msec is designed to be the minimum scheduling granularity, but due to inherent timer precision issues and accounting mechanisms (CONFIG_IRQ_TIME_ACCOUNTING/CONFIG_PARAVIRT_TIME_ACCOUNTING), ticks often complete faster than 1ms. The combination of precise deadline calculation and imprecise tick timing causes the scheduler to grant tasks extra runtime when the tick arrives before the deadline is marked as reached.

## Fix Summary

The fix reduces the default base slice value from 0.75 msec to 0.70 msec, creating additional margin to prevent tasks from exceeding their intended slice due to tick timing errors. This small adjustment (50 microseconds) eliminates the extra tick phenomenon on systems where it occurs, while maintaining acceptable scheduling granularity across all CPU configurations.

## Triggering Conditions

The bug requires HZ=1000 configuration with 8 or more CPUs to trigger the 3.0ms base slice (0.75ms × 4). Tasks must run long enough to approach their vruntime deadline while being subject to tick timing variations. The triggering conditions include: (1) clockevent precision causing ticks to arrive slightly earlier than 1ms intervals, (2) CONFIG_IRQ_TIME_ACCOUNTING or CONFIG_PARAVIRT_TIME_ACCOUNTING reducing actual tick durations, and (3) deadline checks occurring only at tick boundaries. When a task's deadline approaches and the next tick arrives faster than expected, the scheduler perceives the deadline as not yet reached, granting an extra 1ms tick of runtime beyond the intended slice allocation.

## Reproduce Strategy (kSTEP)

Configure 8+ CPUs to ensure 3.0ms base slice. Create multiple competing CFS tasks using kstep_task_create() to trigger slice-based preemption. Set precise tick interval using tick_interval_ns=1000000 (1ms) and monitor timing with on_tick_begin()/on_tick_end() callbacks. Use kstep_tick_repeat() to advance time and observe task runtime behavior around slice boundaries. Track each task's vruntime progression and compare actual runtime against expected slice duration. Log task switches and deadline calculations to detect when tasks receive extra ticks. Success is measured by observing tasks consistently running beyond their 3.0ms slice allocation when the buggy kernel's 0.75ms base slice creates insufficient timing margin for clockevent precision variations.
