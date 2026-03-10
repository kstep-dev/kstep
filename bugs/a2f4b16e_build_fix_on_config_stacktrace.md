# sched_ext: Build fix on !CONFIG_STACKTRACE[_SUPPORT]

- **Commit:** a2f4b16e736d62892ffd333996a7d682b57f6664
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The kernel fails to compile on configurations where `CONFIG_STACKTRACE` is not enabled. The `scx_dump_task()` function unconditionally calls `stack_trace_save_tsk()`, which is only available when `CONFIG_STACKTRACE` is defined, resulting in undefined reference build errors.

## Root Cause

The `scx_dump_task()` function in `kernel/sched/ext.c` calls `stack_trace_save_tsk()` without checking if `CONFIG_STACKTRACE` is enabled. This function is only declared and defined when the config option is present, causing the build to fail on systems without this support.

## Fix Summary

The fix wraps the call to `stack_trace_save_tsk()` with `#ifdef CONFIG_STACKTRACE`, allowing the code to compile on systems without stacktrace support. Additionally, `bt_len` is initialized to 0 to ensure it has a valid value when stacktrace capture is skipped.

## Triggering Conditions

This is a build-time compilation error, not a runtime scheduling bug. The error occurs when:
- `CONFIG_SCHED_CLASS_EXT` is enabled (sched_ext scheduling class)
- `CONFIG_STACKTRACE` is disabled or `STACKTRACE_SUPPORT` is unavailable on the target architecture
- The kernel build process attempts to compile `kernel/sched/ext.c`
- The `scx_dump_task()` function references the undefined `stack_trace_save_tsk()` symbol
- This typically happens during sched_ext error handling/debugging when task dumps are generated
- Affects kernel builds on architectures without stacktrace support or embedded configurations

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced through runtime kSTEP testing since it's a build-time compilation failure. Instead, reproduction would require:
- Building a kernel configuration with `CONFIG_SCHED_CLASS_EXT=y` and `CONFIG_STACKTRACE=n`
- Attempting to compile `kernel/sched/ext.c` which would fail with undefined reference errors
- A kSTEP driver could theoretically trigger the `scx_dump_task()` code path by causing sched_ext errors, but this requires a successfully built kernel with sched_ext support
- The reproduction strategy would involve: configuring a minimal kernel build, disabling stacktrace support, enabling sched_ext, and observing the compilation failure
- For runtime testing: use `kstep_task_create()` with sched_ext policies to trigger error paths that call `scx_dump_task()`
