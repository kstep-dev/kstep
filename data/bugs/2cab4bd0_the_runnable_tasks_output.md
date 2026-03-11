# sched/debug: Fix the runnable tasks output

- **Commit:** 2cab4bd024d23f658e40dce209dfd012f4e8b19a
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core

## Bug Description

The runnable tasks debug output was malformed and contained a duplicate printing of `sum_exec_runtime`, making the output confusing and difficult to parse. The duplicate field appeared twice in the second SEQ_printf call, resulting in misaligned columns and unclear information when users examined the scheduler's debug output.

## Root Cause

The `print_task()` function in `kernel/sched/debug.c` was printing `sum_exec_runtime` twice: once in the first SEQ_printf call and again in the second SEQ_printf call. This duplication occurred when the second SEQ_printf statement printed four fields instead of three, with the second field being a redundant copy of `sum_exec_runtime`.

## Fix Summary

The fix removes the duplicate `sum_exec_runtime` printing from the second SEQ_printf call, keeping only the original in the first call. Additionally, it reorganizes the output columns and header formatting to be cleaner and more descriptive, properly aligning all fields and adding clearer column names (vruntime, eligible, deadline, slice, sum-exec, etc.).

## Triggering Conditions

This bug manifests whenever the scheduler debug information is accessed, specifically when `/proc/sched_debug` is read or `kstep_print_sched_debug()` is called. The issue occurs in the `print_task()` function which formats output for each runnable task on every CPU's runqueue. No specific scheduler state, cgroup configuration, or CPU topology is required - the bug appears with any system that has runnable tasks. The duplicate `sum_exec_runtime` field appears in both the first and second `SEQ_printf` calls, with the second call incorrectly printing four fields instead of three, causing column misalignment and confusing output formatting.

## Reproduce Strategy (kSTEP)

Use any CPU count (minimum 2 for kSTEP). In `setup()`, create 2-3 tasks with `kstep_task_create()` and optionally pin them to different CPUs using `kstep_task_pin()`. In `run()`, wake the tasks with `kstep_task_wakeup()` and run a few ticks with `kstep_tick_repeat(10)` to get them scheduled. Call `kstep_print_sched_debug()` to generate the debug output and examine the runnable tasks section. The bug is visible by looking for duplicate `sum_exec_runtime` values in the output columns - the value should appear twice for each task entry, once in the correct position and once misplaced. Check the log output for malformed columns and misaligned fields. No specific callbacks are needed, though `on_tick_begin` with `kstep_output_nr_running()` can help verify tasks are running.
