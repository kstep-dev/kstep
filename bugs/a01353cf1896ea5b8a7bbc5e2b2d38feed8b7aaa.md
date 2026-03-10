# cpuidle: Fix ct_idle_*() usage

- **Commit:** a01353cf1896ea5b8a7bbc5e2b2d38feed8b7aaa
- **Affected file(s):** kernel/sched/idle.c
- **Subsystem:** Core (cpuidle/RCU interaction)

## Bug Description

The cpuidle code had an incorrect sequence for managing RCU and IRQ state during the idle enter/exit dance. The manual sequence of `ct_idle_enter()` followed by `local_irq_enable()`, and the reverse on exit with manual `lockdep_hardirqs_*()` calls, could fail to correctly maintain the interaction between RCU state and IRQ tracing. This could lead to RCU stalls, incorrect lockdep state, or tracing issues when entering or exiting idle states.

## Root Cause

The disable-RCU, enable-IRQS dance is intricate because changing IRQ state is traced, which itself depends on RCU being properly disabled. The original code attempted to manually manage this using lockdep and tracing calls (`lockdep_hardirqs_on_prepare()`, `lockdep_hardirqs_on()`, `lockdep_hardirqs_off()`), but this manual sequence was incomplete and error-prone. The correct sequence was not being followed in all cases, particularly in `cpu_idle_poll()` and `default_idle_call()`.

## Fix Summary

The fix introduces two new helper functions, `ct_cpuidle_enter()` and `ct_cpuidle_exit()`, that correctly mirror the entry code's handling of the RCU/IRQ state transition. These helpers encapsulate the correct sequence of operations and are used to replace all buggy instances of the manual enter/exit dance in the idle path, ensuring consistent and correct RCU and IRQ state management during cpuidle operations.
