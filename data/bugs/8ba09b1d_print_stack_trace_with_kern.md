# sched: print stack trace with KERN_INFO

- **Commit:** 8ba09b1dc131ff9bb530967c593e298c600f72c0
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The backtrace printed by `sched_show_task()` was being emitted at an inconsistent log level compared to other diagnostic messages in the same function. While other messages in `sched_show_task()` use KERN_INFO or KERN_CONT, the stack trace was printed at the default log level (likely KERN_DEFAULT), causing it to be handled differently by the logging system and potentially filtered out or appear out of order in logs.

## Root Cause

The function `show_stack(p, NULL)` was called without specifying a log level parameter, whereas the preceding `print_worker_info()` call explicitly used `KERN_INFO`. The original `show_stack()` function does not accept a log level parameter, causing the backtrace to use a default log level instead of being aligned with the rest of the diagnostic output.

## Fix Summary

Changed `show_stack(p, NULL)` to `show_stack_loglvl(p, NULL, KERN_INFO)` to explicitly specify the KERN_INFO log level. This ensures the stack trace is printed at the same log level as the other messages in `sched_show_task()`, maintaining consistent formatting and filtering behavior for diagnostic output.

## Triggering Conditions

This bug manifests whenever `sched_show_task()` is called to print task diagnostic information. The function is typically invoked during:
- System hang detection (hung task detector)
- Kernel panic or oops handling where task states are dumped
- Manual invocation via `/proc/sysrq-trigger` with 't' (show task states)
- Out-of-memory (OOM) killer reporting
- Any debugging scenario where task information needs to be displayed

The inconsistent log levels cause the stack trace portion to potentially be filtered differently than the task state information, making logs incomplete or fragmented when certain log level filters are applied.

## Reproduce Strategy (kSTEP)

This bug is primarily a logging consistency issue that doesn't affect scheduler behavior but impacts diagnostic output visibility. To reproduce:

**Setup:** Requires at least 1 CPU (CPU 0 reserved for driver). Create a task that can trigger diagnostic output.

**Steps in run():**
1. Use `kstep_task_create()` to create a test task
2. Call `kstep_task_pause()` to put task in TASK_UNINTERRUPTIBLE state
3. Directly invoke `sched_show_task()` via imported symbol to trigger the diagnostic output
4. Capture and analyze the kernel log output using appropriate log level filters

**Detection:** Compare log output between buggy and fixed kernels. In the buggy version, the stack trace portion may be missing or appear at different times when KERN_INFO messages are filtered, while in the fixed version all output appears consistently at KERN_INFO level.
