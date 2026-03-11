# sched_ext: Fix incorrect sched_class settings for per-cpu migration tasks

- **Commit:** 1dd6c84f1c544e552848a8968599220bd464e338
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

When the sched_ext BPF scheduler is enabled or disabled, tasks in the scx_tasks list are traversed and have their scheduler class updated via `__setscheduler_class()`. However, this incorrectly modifies per-cpu migration tasks' sched_class from `stop_sched_class` to `rt_sched_class`. Critically, this change persists even after the BPF scheduler is unloaded, leaving migration tasks in a corrupted state with an incorrect scheduler class.

## Root Cause

The code unconditionally applied `__setscheduler_class(p->policy, p->prio)` to all tasks without checking their existing scheduler class. Per-cpu migration tasks are special kernel tasks that must always use `stop_sched_class` to preserve their critical CPU migration functionality. The original code lacked a guard to protect these tasks from scheduler class modifications.

## Fix Summary

A new helper function `scx_setscheduler_class()` is introduced that checks if a task's current sched_class is `stop_sched_class` and, if so, returns it unchanged rather than calling `__setscheduler_class()`. This function is used in both the enable path (scx_enable) and disable path (scx_disable_workfn) to ensure migration tasks retain their proper scheduler class throughout sched_ext operations.

## Triggering Conditions

The bug triggers when sched_ext BPF scheduler enable/disable operations unconditionally apply `__setscheduler_class()` to all tasks in the scx_tasks list. Per-cpu migration tasks (with `stop_sched_class`) are incorrectly changed to `rt_sched_class` during both enable and disable paths. The corruption persists because migration tasks' scheduler class is not restored when the BPF scheduler is unloaded. Critical conditions: per-cpu migration tasks must exist (normal on SMP systems), and any sched_ext BPF scheduler load/unload operation occurs. The bug affects all CPUs as each has its own migration/N kernel thread.

## Reproduce Strategy (kSTEP)

Multi-CPU setup (minimum 2 CPUs, CPU 0 reserved for driver). In `setup()`, use `kstep_topo_init()` and `kstep_topo_apply()` to ensure multiple CPUs are available. In `run()`, examine migration tasks' sched_class before simulating sched_ext operations. Since kSTEP cannot directly load BPF schedulers, simulate the problematic code path by directly accessing migration task structures and checking their `p->sched_class` field. Use `on_tick_begin()` callback to log migration task states across multiple CPUs. Check if migration/N tasks have `stop_sched_class` initially, then verify if they incorrectly change to `rt_sched_class` during scheduler transitions. Detection: log each migration task's sched_class pointer and verify it remains `&stop_sched_class` throughout operations.
