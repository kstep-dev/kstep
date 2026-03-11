# sched/core: Prevent rescheduling when interrupts are disabled

- **Commit:** 82c387ef7568c0d96a918a5a78d9cad6256cfa15
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

During kexec jump testing with interrupts disabled, a syscore_suspend() callback wakes up a task, which sets the NEED_RESCHED flag. A later callback in the resume path invokes cond_resched(), which in turn calls schedule() from a context with interrupts disabled. The scheduler then enables interrupts after context switching, causing a warning at syscore_resume() completion. This affects PREEMPT_NONE and PREEMPT_VOLUNTARY scheduling models.

## Root Cause

The `__cond_resched()` function checks only if rescheduling is needed via `should_resched(0)` but does not verify whether interrupts are disabled. When a task wakeup occurs in an interrupt-disabled section, the NEED_RESCHED flag is set, and cond_resched() subsequently invokes schedule() despite interrupts being disabled, leading to the scheduler enabling interrupts in an incorrect context.

## Fix Summary

The fix adds an additional check `&& !irqs_disabled()` to the condition in `__cond_resched()`, preventing the scheduler from being invoked when interrupts are disabled. This ensures schedule() is only called from safe execution contexts where interrupt state can be properly managed.

## Triggering Conditions

The bug requires the following precise sequence in PREEMPT_NONE or PREEMPT_VOLUNTARY kernels:
- A task wakeup occurs while in an interrupt-disabled critical section (e.g., during syscore_suspend/resume)
- This wakeup sets the NEED_RESCHED flag on the current task
- Later in the same interrupt-disabled section, `cond_resched()` is called (e.g., from ACPI operations, mutex operations)
- The `__cond_resched()` function sees NEED_RESCHED set and calls `schedule()` despite interrupts being disabled
- The scheduler enables interrupts after context switching, violating the interrupt-disabled invariant
- This triggers warnings about unexpected interrupt enablement in syscore operations

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. Create a scenario that mimics syscore operations with interrupt-disabled sections:
- In `setup()`: Create 2 tasks using `kstep_task_create()` - one "waker" task and one "target" task
- In `run()`: Disable interrupts using a kernel mechanism that calls `local_irq_disable()`
- Have the waker task perform operations that trigger a wakeup of the target task (setting NEED_RESCHED)
- While still in interrupt-disabled context, call functions that invoke `cond_resched()` (e.g., mutex operations)
- Use `on_tick_begin()` callback to monitor interrupt state and task flags
- Detect the bug by checking if interrupts get enabled when they should remain disabled
- Log when `__cond_resched()` is called with NEED_RESCHED set and interrupts disabled
- Verify the fix by confirming `cond_resched()` returns early when interrupts are disabled
