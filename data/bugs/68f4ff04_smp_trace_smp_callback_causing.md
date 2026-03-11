# sched, smp: Trace smp callback causing an IPI

- **Commit:** 68f4ff04dbada18dad79659c266a8e5e29e458cd
- **Affected file(s):** kernel/sched/core.c, kernel/sched/smp.h
- **Subsystem:** core, SMP

## Bug Description

The `ipi_send_cpumask` tracepoint was unable to capture the callback function for all CSD (call_single_data) types. For CSD_TYPE_TTWU, there is no callback function attached to the struct, and the CSD type flags are cleared before the callback is executed, making it impossible to reliably extract and trace the callback information. This resulted in the tracepoint always being fed with NULL for the callback parameter, defeating the purpose of tracing.

## Root Cause

The original `send_call_function_single_ipi()` function was defined in sched/core.c and combined scheduler-specific logic with IPI emission, making it impossible to pass the callback function as a parameter to the tracepoint without creating unnecessary overhead. The CSD type information would be cleared before the tracepoint could access it, and there was no architectural way to thread the callback information through without major refactoring.

## Fix Summary

The fix refactors `send_call_function_single_ipi()` by splitting it into two parts: `call_function_single_prep_ipi()` is kept in sched/core.c to handle scheduler-specific checks (like `set_nr_if_polling()`), while the actual IPI emission logic is moved to smp.c. This separation allows the callback function to be passed as a parameter and properly traced by the `ipi_send_cpumask` tracepoint without creating register pressure or overhead in the non-traced code path.

## Triggering Conditions

This is a tracing infrastructure issue, not a functional scheduler bug. The problem occurs whenever:
- The `ipi_send_cpumask` tracepoint is enabled during SMP call operations
- Any CSD (call_single_data) operations are performed, especially CSD_TYPE_TTWU (task wakeup IPIs)
- The tracepoint attempts to extract the callback function from the CSD structure after flags are cleared
- Remote task wakeups or any smp_call_function_single() operations are executed
- The system attempts to trace the IPI callback function but receives NULL due to architectural limitations

## Reproduce Strategy (kSTEP)

This tracing issue cannot be reproduced through functional behavior, as the scheduler works correctly but simply fails to properly trace IPI callbacks. To verify the fix through observable behavior:
- Setup: Enable the `ipi_send_cpumask` tracepoint in the kernel, require 2+ CPUs  
- Create tasks using `kstep_task_create()` and pin them to different CPUs with `kstep_task_pin()`
- Use `kstep_task_wakeup()` to trigger remote task wakeups that generate CSD_TYPE_TTWU IPIs
- Add `smp_call_function_single()` operations to generate other CSD types
- Monitor tracepoint output to verify callback functions are properly captured (not NULL)
- Use callbacks like `on_tick_begin()` to log when IPIs are sent between CPUs
- Check that the tracepoint now receives actual function pointers instead of NULL values
