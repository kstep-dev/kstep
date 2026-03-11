# sched/debug: Correct printing for rq->nr_uninterruptible

- **Commit:** a6fcdd8d95f7486150b3faadfea119fc3dfc3b74
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core (scheduler debug)

## Bug Description

When `rq->nr_uninterruptible` (type: `unsigned int`) was printed using a `long` cast and `%ld` format specifier, the debug output displayed incorrect values. For example, a value of `0xfffffff7` (4,294,967,287 as unsigned int) would be printed as 4,294,967,287 instead of the correctly interpreted signed value of -9. This resulted in misleading debug output in `/sys/kernel/debug/sched/debug` and console logs.

## Root Cause

A previous commit (e6fe3f422be1) changed the type of `rq->nr_uninterruptible` from `unsigned long` to `unsigned int`, but the print statement in the debug macro was not updated. The code continued to use `(long)` cast with `%ld` format specifier, causing incorrect interpretation and display of 32-bit unsigned values when printed as 64-bit signed longs.

## Fix Summary

Changed the cast and format specifier from `(long)` and `%ld` to `(int)` and `%d` for 4-byte fields in the debug print macro. This ensures that 32-bit unsigned integers are correctly cast and printed with the appropriate format specifier.

## Triggering Conditions

The bug occurs when `rq->nr_uninterruptible` has its high bit set (values like 0xfffffff7), representing a negative count in two's complement. This happens when tasks transition from uninterruptible sleep states in specific patterns that cause the counter to underflow. The incorrect debug output is triggered when the scheduler debug information is printed via `/sys/kernel/debug/sched/debug` or console debug prints. The bug manifests in the `print_cpu()` function in `kernel/sched/debug.c`, specifically in the `P(x)` macro that handles 4-byte field printing. Any kernel debugging operation that calls `kstep_print_sched_debug()` or accesses the debug filesystem will expose the incorrect formatting.

## Reproduce Strategy (kSTEP)

Set up 2 CPUs (CPU 0 reserved, use CPUs 1-2). In `setup()`, create several tasks using `kstep_task_create()` and configure them to transition between runnable and uninterruptible sleep states. In `run()`, manipulate task states to artificially create negative `nr_uninterruptible` values by having more tasks wake up from uninterruptible sleep than enter it within a short time window. Use `kstep_task_pause()` followed by `kstep_task_wakeup()` in patterns that affect the uninterruptible counter. Call `kstep_print_sched_debug()` to trigger debug output and capture the formatted values. Compare the printed output with the actual raw value of `rq->nr_uninterruptible` to detect incorrect formatting. Use `on_tick_end` callback to monitor counter state changes and log when the high bit becomes set, indicating the bug condition is present.
