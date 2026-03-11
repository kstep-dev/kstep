# sched/cpuacct: Fix charge cpuacct.usage_sys

- **Commit:** dbe9337109c2705f08e6a00392f991eb2d2570a5
- **Affected file(s):** kernel/sched/cpuacct.c
- **Subsystem:** cpuacct

## Bug Description

The cpuacct.usage_sys (kernel mode CPU time) accounting is computed incorrectly. The function `cpuacct_charge()` uses `task_pt_regs(tsk)` to determine whether CPU time should be charged to user mode or kernel mode, but this register state does not reflect the actual CPU mode when called from interrupt/exception handlers. This causes kernel-mode time spent in interrupt handlers to be incorrectly attributed to system accounting.

## Root Cause

The `task_pt_regs()` function returns the task's saved register state, which for user-space threads always indicates user mode (since it was saved when entering the kernel). However, when `cpuacct_charge()` is invoked from interrupt or exception handlers, this register state does not reflect the actual CPU mode at the time of the interrupt. The function should use the current interrupt register state (via `get_irq_regs()`) to correctly determine the CPU mode, as this represents the actual execution context when the charge occurs.

## Fix Summary

The fix changes `cpuacct_charge()` to first attempt to get the current interrupt register state via `get_irq_regs()`, and only fall back to `task_pt_regs()` when not in an interrupt context. This ensures that the correct register state is checked when determining whether to charge CPU time to user or kernel mode accounting, fixing the misattribution of interrupt handler execution time.

## Triggering Conditions

- **cpuacct subsystem:** Active with cgroup accounting enabled
- **Task context:** User-space task (so `task_pt_regs()` returns user-mode registers)
- **Interrupt timing:** `cpuacct_charge()` called from interrupt or exception handler context
- **Execution path:** CPU time accounting triggered while handling interrupts/exceptions that occur during user-space execution
- **Observable symptom:** System time (`cpuacct.usage_sys`) incorrectly accumulates interrupt handler time as user time instead of kernel time

## Reproduce Strategy (kSTEP)

- **CPUs needed:** 2 (CPU 0 reserved for driver, CPU 1 for test task)
- **Setup:** Create cpuacct cgroup with `kstep_cgroup_create("test_acct")` and configure task pinning
- **Test sequence:** Create user task with `kstep_task_create()`, pin to CPU 1 with `kstep_task_pin()`, add to cgroup with `kstep_cgroup_add_task()`
- **Trigger conditions:** Use `kstep_tick_repeat()` extensively to generate timer interrupts during user task execution
- **Detection method:** Read cpuacct usage before/after with `kstep_cgroup_write()` to access usage_sys statistics
- **Bug verification:** Compare system time accounting - buggy kernel incorrectly attributes interrupt time to user accounting when called from interrupt context
- **Callback usage:** Use `on_tick_begin()` to log cgroup stats and verify incorrect time attribution patterns
