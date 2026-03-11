# sched/debug: Show the registers of 'current' in dump_cpu_task()

- **Commit:** bc1cca97e6da6c7c34db7c5b864bb354ca5305ac
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The `dump_cpu_task()` function fails to display useful register information when dumping the current CPU's stack on architectures that do not support NMIs (such as ARM64). This results in a backtrace showing only the dump function's own call chain, which is worthless for debugging. When RCU stalls occur on such systems, investigators cannot see what the task was actually executing at the time of the stall.

## Root Cause

`dump_cpu_task()` relies on `trigger_single_cpu_backtrace()` to dump task information, which depends on NMI support. On non-NMI architectures, this function cannot obtain the actual CPU registers of the task being dumped. When called from interrupt context for the current CPU, the function ignores an available opportunity to use `get_irq_regs()` to retrieve the actual interrupt context registers.

## Fix Summary

The fix adds a check at the start of `dump_cpu_task()`: if the function is being called from interrupt context (`in_hardirq()`) to dump the current CPU (`cpu == smp_processor_id()`), it uses `get_irq_regs()` to retrieve and display the actual registers of the current task. This provides useful debugging information on non-NMI systems and avoids self-NMI on systems that support NMIs.

## Triggering Conditions

The bug manifests during debugging scenarios on non-NMI architectures (ARM64, RISC-V) when:
- RCU stall detection triggers `dump_cpu_task()` from interrupt context (timer interrupt)
- The dump is for the current CPU (`cpu == smp_processor_id()`)
- Task is running in user space or specific kernel functions when interrupt occurs
- Without the fix, only the interrupt handler's call stack is shown, obscuring the actual task state
- The condition requires a long-running task that triggers RCU stall detection

## Reproduce Strategy (kSTEP)

Create a long-running task that triggers RCU stall detection:
1. Use 2 CPUs (CPU 0 reserved for driver, CPU 1 for test task)
2. In `setup()`: Create one task with `kstep_task_create()` and pin to CPU 1
3. In `run()`: Use `kstep_task_wakeup()` and busy-loop via repeated `kstep_tick()`
4. Configure RCU stall timeout via `kstep_sysctl_write("kernel.rcu_cpu_stall_timeout", "5")`
5. Monitor logs for RCU stall detection and examine `dump_cpu_task()` output
6. Compare register information quality: fixed version shows actual task registers, buggy version shows only interrupt handler stack
7. Look for "Call trace:" sections - buggy version shows dump functions, fixed shows actual task execution
