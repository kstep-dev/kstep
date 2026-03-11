# Trigger warning if ->migration_disabled counter underflows

- **Commit:** 9d0df37797453f168afdb2e6fd0353c73718ae9a
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

If `migrate_enable()` is called more times than its counterpart `migrate_disable()`, the `migration_disabled` counter underflows silently without detection. This causes `rq::nr_pinned` to also underflow, corrupting the pinned task counter and leading to incorrect scheduling decisions. The bug remains hidden because the counter decrement proceeds unconditionally.

## Root Cause

The `migrate_enable()` function decrements `p->migration_disabled` and `this_rq()->nr_pinned` without checking whether these counters are in a valid state (non-zero). When `migrate_enable()` is called without a matching `migrate_disable()`, the counters underflow, but this corruption goes undetected since there is no validation of the precondition.

## Fix Summary

The fix adds a `WARN_ON_ONCE(!p->migration_disabled)` check before decrementing the counter. If `migration_disabled` is already 0, the warning triggers and the function returns early, preventing the underflow of both `migration_disabled` and `nr_pinned`. This makes mismatched enable/disable calls immediately detectable.

## Triggering Conditions

The bug requires calling `migrate_enable()` more times than `migrate_disable()` on the same task:
- Task must have `migration_disabled` counter at 0 (initial state or after balanced calls)
- Subsequent `migrate_enable()` call attempts to decrement from 0, causing underflow
- The `rq::nr_pinned` counter also underflows when `this_rq()->nr_pinned--` executes
- Bug occurs in core scheduler subsystem during migration control operations
- No specific CPU topology, cgroup configuration, or task state requirements
- Race conditions not involved - purely a counter management logic error
- Detection requires kernel compiled with appropriate warning mechanisms enabled

## Reproduce Strategy (kSTEP)

Requires minimal setup with 2+ CPUs (CPU 0 reserved for driver):
- Setup: Create a single task with `kstep_task_create()`, wake it with `kstep_task_wakeup()`
- Run sequence: Directly call kernel migration functions to trigger the bug condition
- Access task's `migration_disabled` field and current CPU's `nr_pinned` counter directly
- Call `migrate_enable()` on a task that hasn't called `migrate_disable()` (counter = 0)
- Use `on_tick_begin()` callback to log counter values before/after the erroneous call
- Detection: Check for kernel warning message in dmesg/printk output, verify counters wrap to maximum values
- Log task's `p->migration_disabled` and current CPU's `rq->nr_pinned` to confirm underflow
- Alternative: Use kthread with `kstep_kthread_create()` and manipulate migration state directly
