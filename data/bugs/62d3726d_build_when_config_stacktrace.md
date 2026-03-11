# sched_ext: Fix build when !CONFIG_STACKTRACE

- **Commit:** 62d3726d4cd66f3e48dfe0f0401e0d74e58c2170
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The kernel fails to build when `CONFIG_STACKTRACE` is disabled. Specifically, calls to `stack_trace_print()` and `stack_trace_save()` are compiled unconditionally, even though these functions are only available when `CONFIG_STACKTRACE` is enabled. This results in undefined reference/compilation errors on configurations with `!CONFIG_STACKTRACE`.

## Root Cause

The functions `stack_trace_print()` and `stack_trace_save()` are only defined when `CONFIG_STACKTRACE` is set. However, in `kernel/sched/ext.c`, these functions were being called unconditionally in two locations: `scx_ops_disable_workfn()` and `scx_ops_exit_kind()`. A previous build fix attempt (a2f4b16e736d) did not fully address the issue.

## Fix Summary

The fix wraps the calls to `stack_trace_print()` and `stack_trace_save()` with `#ifdef CONFIG_STACKTRACE` preprocessor directives. This prevents the compiler from attempting to compile these function calls when the stacktrace functionality is not available, resolving the build failure on configurations with `!CONFIG_STACKTRACE`.

## Triggering Conditions

This is a compile-time bug triggered by specific kernel build configuration:
- Kernel built with `!CONFIG_STACKTRACE` (stacktrace support disabled)
- `CONFIG_SCHED_CLASS_EXT` enabled (sched_ext BPF scheduler support)
- Compilation of `kernel/sched/ext.c` fails due to undefined references to `stack_trace_print()` and `stack_trace_save()`
- The bug manifests in error handling paths within `scx_ops_disable_workfn()` and `scx_ops_exit_kind()` functions
- Specifically affects the code path where BPF schedulers encounter errors (`ei->kind >= SCX_EXIT_ERROR`) and attempt to print diagnostic stack traces

## Reproduce Strategy (kSTEP)

Since this is a build-time issue, runtime reproduction focuses on exercising the error paths that contain the problematic code:
- **Setup**: Enable sched_ext with a minimal BPF scheduler, requires 2+ CPUs (CPU 0 reserved)
- **Strategy**: Force BPF scheduler errors to trigger `scx_ops_disable_workfn()` error handling
- **Implementation**: Use `kstep_task_create()` and `kstep_cgroup_create()` to create scheduler stress, then cause deliberate BPF scheduler failures through resource exhaustion or invalid operations
- **Detection**: Monitor kernel logs for "sched_ext: BPF scheduler disabled" error messages via `on_tick_end()` callback
- **Verification**: Confirm error paths execute (though stack trace calls would be conditionally compiled out in the fix)
