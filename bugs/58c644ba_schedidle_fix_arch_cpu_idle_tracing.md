# sched/idle: Fix arch_cpu_idle() vs tracing

- **Commit:** 58c644ba512cfbc2e39b758dd979edd1d6d00e27
- **Affected file(s):** kernel/sched/idle.c
- **Subsystem:** core

## Bug Description

When `arch_cpu_idle()` is called, RCU is disabled via `rcu_idle_enter()`, but architecture-specific implementations may attempt to enable interrupts. Enabling interrupts invokes the tracing infrastructure (via `local_irq_enable()`), which depends on RCU being enabled. This creates a conflict where tracing code runs while RCU is disabled, causing tracing and RCU interactions to malfunction.

## Root Cause

The code path in `default_idle_call()` calls `rcu_idle_enter()` which disables RCU, then invokes `arch_cpu_idle()` which is expected to enable interrupts. The original implementation used `local_irq_enable()`, which invokes tracing code that requires RCU to be enabled, creating an unsupported state where tracing executes with RCU disabled.

## Fix Summary

The fix changes `arch_cpu_idle()` to use `raw_local_irq_enable()` instead of `local_irq_enable()` to avoid invoking tracing. Additionally, `default_idle_call()` explicitly manages tracing, lockdep, and RCU state around the `arch_cpu_idle()` call, using `trace_hardirqs_on_prepare()` before disabling RCU and carefully re-enabling hardware IRQ tracking afterward with `raw_local_irq_disable()` and `lockdep_hardirqs_off()`.

## Triggering Conditions

- A CPU must go idle (no runnable tasks) to invoke the `default_idle_call()` path in `kernel/sched/idle.c`
- The kernel must have tracing enabled (`CONFIG_TRACING`) to trigger the problematic interaction
- RCU is disabled via `rcu_idle_enter()` before calling `arch_cpu_idle()`
- The architecture-specific `arch_cpu_idle()` implementation calls `local_irq_enable()` which invokes tracing infrastructure
- This creates an illegal state where tracing code executes while RCU is disabled, causing RCU warnings or tracing malfunctions
- The race window exists between `rcu_idle_enter()` and the interrupt enable in `arch_cpu_idle()`

## Reproduce Strategy (kSTEP)

- Setup: Use 2 CPUs minimum (CPU 0 reserved for driver). Enable tracing if not default.
- Create a single task pinned to CPU 1 using `kstep_task_create()` and `kstep_task_pin(task, 1, 1)`
- In `setup()`: Configure any required tracing/debugging options via `kstep_sysctl_write()`
- In `run()`: Start the task with `kstep_task_wakeup(task)`, let it run briefly with `kstep_tick_repeat(10)`
- Force CPU 1 idle: Pause the task with `kstep_task_pause(task)`, then advance ticks with `kstep_tick_repeat(50)`
- Use `on_tick_begin()` callback to monitor when CPU 1 goes idle and enters `default_idle_call()`
- Monitor for RCU warnings in kernel logs or tracing subsystem errors during idle entry
- Detection: Check for RCU splat messages or tracing-related warnings when CPU 1 transitions to idle
- Verify fix: The same sequence should not generate warnings on the patched kernel
