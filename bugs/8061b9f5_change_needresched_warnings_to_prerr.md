# sched/debug: Change need_resched warnings to pr_err

- **Commit:** 8061b9f5e111a3012f8b691e5b75dd81eafbb793
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core

## Bug Description

Need_resched debug warnings are issued when the scheduler detects that need_resched has been set for too long without a schedule event occurring. When kernel.panic_on_warn is enabled, these WARN() calls trigger a kernel panic, causing catastrophic system failure. This is undesirable behavior since these warnings are meant for debugging scheduler latency issues, not for triggering panics.

## Root Cause

The resched_latency_warn() function uses the WARN() macro to emit debug messages about scheduler issues. The WARN() macro is treated as a kernel warning, which causes kernel panic when panic_on_warn is enabled. This creates an unintended interaction where a debug diagnostic mechanism becomes a panic trigger in certain kernel configurations.

## Fix Summary

The fix replaces the WARN() macro call with a pr_err() printk followed by dump_stack(). This changes the message from a WARNING (which triggers panic_on_warn) to a regular error message with a stack trace, achieving the original debugging goal without triggering unwanted panics.

## Triggering Conditions

- A task must have `need_resched` flag set for an extended period (> threshold ns) without scheduling
- The `resched_latency_warn()` function is called from scheduler tick processing when `ticks_without_resched` exceeds limits
- Rate limiting allows one warning per hour maximum to prevent spam
- System must have `kernel.panic_on_warn=1` sysctl setting to trigger the panic behavior
- High CPU load or long-running tasks in kernel space that don't voluntarily schedule can create this condition
- The bug manifests as an unexpected kernel panic instead of a debug warning message

## Reproduce Strategy (kSTEP)

- Use 2+ CPUs (CPU 0 reserved for driver, test on CPU 1)
- In `setup()`: Enable panic_on_warn with `kstep_sysctl_write("kernel.panic_on_warn", "1")`
- Create a high-priority SCHED_FIFO task with `kstep_task_create()` and `kstep_task_fifo()`
- Pin the task to CPU 1 with `kstep_task_pin()` and wake it with `kstep_task_wakeup()`
- In `run()`: Use `kstep_tick_repeat()` to advance many ticks without allowing the FIFO task to schedule
- Force `need_resched` to remain set by keeping the task runnable but not current
- Monitor for the panic trigger by checking kernel messages or system state
- The bug is detected when the system panics due to WARN() instead of printing a debug message
- Alternatively, patch kernel to log WARN() calls to detect when `resched_latency_warn()` triggers
