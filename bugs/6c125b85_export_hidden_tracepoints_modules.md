# sched: Export hidden tracepoints to modules

- **Commit:** 6c125b85f3c87b4bf7dba91af6f27d9600b9dba0
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

Three scheduler tracepoints (`sched_entry`, `sched_exit`, and `sched_set_need_resched`) were not exported to modules after being adapted for the RV task model. This prevented external debugging and tracing modules from accessing these tracepoints via ftrace, breaking compatibility with existing tools that rely on them for scheduler debugging.

## Root Cause

When the sched tracepoints were adapted in a previous commit (adcc3bfa8806), the corresponding `EXPORT_TRACEPOINT_SYMBOL_GPL()` declarations were not added for the three new/modified tracepoints, while other scheduler tracepoints were properly exported. Without these export declarations, kernel modules cannot register probes on these tracepoints.

## Fix Summary

The fix adds three `EXPORT_TRACEPOINT_SYMBOL_GPL()` declarations for `sched_entry_tp`, `sched_exit_tp`, and `sched_set_need_resched_tp`, making them available to external kernel modules and debugging tools like sched_tp that depend on these tracepoints.

## Triggering Conditions

This is not a runtime scheduler behavior bug but a module loading/symbol availability issue. The bug triggers when:
- A kernel module attempts to register probes on `sched_entry`, `sched_exit`, or `sched_set_need_resched` tracepoints
- The module calls functions like `register_trace_sched_entry()` but the symbols are not exported
- Module loading fails with "Unknown symbol" errors for the missing tracepoint exports
- External debugging tools (like sched_tp) cannot access these specific tracepoints via ftrace
- The issue occurs at module load time, not during scheduler execution

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced using the kSTEP framework's runtime APIs since it's a compile-time/module-loading issue, not a scheduler execution bug. To demonstrate the issue:
- Create a simple kernel module that registers probes on the affected tracepoints
- Use `kstep_write()` to create the module source in `/tmp/test_tracepoint_module.c`
- Include calls to `register_trace_sched_entry()`, `register_trace_sched_exit()`, and `register_trace_sched_set_need_resched()`
- Compile the module against the buggy kernel and attempt to load it
- On the buggy kernel, module loading should fail with undefined symbol errors
- On the fixed kernel, the module should load successfully
- This requires building and testing kernel modules rather than using kSTEP's task simulation APIs
