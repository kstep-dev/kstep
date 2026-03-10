# sched_ext: Validate prev_cpu in scx_bpf_select_cpu_dfl()

- **Commit:** 8a9b1585e2bf2a4d335774c893f5e80cf9262b6d
- **Affected file(s):** kernel/sched/ext.c, kernel/sched/ext_idle.c
- **Subsystem:** EXT (extensible scheduler)

## Bug Description

The sched_ext scheduler was using an unreliable variable `prev_on_scx` to determine whether the previous task was managed by the SCX scheduler. This could lead to incorrect decision-making in the `pick_task_scx()` function regarding whether to keep the previous task running or transition to a different task, potentially causing incorrect scheduling behavior and stalls.

## Root Cause

The original code relied on a `prev_on_scx` variable that didn't accurately reflect the current state of whether the previous task is queued in the SCX scheduler. The condition was prone to inconsistency, as the SCX_RQ_BAL_PENDING flag could be set when a task is not actually on SCX, leading to incorrect validation during task selection.

## Fix Summary

The fix replaces the unreliable `prev_on_scx` variable check with direct validation using `prev->scx.flags & SCX_TASK_QUEUED` to check if the task is queued in the SCX scheduler, and `prev->sched_class != &ext_sched_class` to verify the scheduler class. It also adds event tracking via `__scx_add_event(SCX_EV_ENQ_SLICE_DFL, 1)` for better observability of default slice assignments.

## Triggering Conditions

The bug is triggered when a BPF scheduler calls `scx_bpf_select_cpu_dfl()` from within its `ops.select_cpu()` implementation and provides an invalid `prev_cpu` value that is outside the range `[0, nr_cpu_ids)`. This occurs during task wakeup or migration when the BPF scheduler attempts to select a CPU for a task. The invalid CPU ID bypasses range checks and can lead to out-of-bounds memory accesses in subsequent kernel functions that assume valid CPU indices. The condition requires built-in CPU selection to be enabled (either missing `ops.update_idle()` or `SCX_OPS_KEEP_BUILTIN_IDLE` set).

## Reproduce Strategy (kSTEP)

To reproduce this bug in kSTEP, we need to simulate a BPF scheduler providing an invalid `prev_cpu`. Since kSTEP doesn't directly expose BPF scheduler interfaces, we can create a test that:
1. Setup: Use 2+ CPUs and enable sched_ext with `sched_ext=1` kernel parameter
2. Create tasks using `kstep_task_create()` and pin them to specific CPUs with `kstep_task_pin()`
3. In `run()`: Create a scenario where internal scheduler state gets corrupted to have invalid CPU IDs by manipulating task migration between invalid CPU ranges
4. Use `kstep_task_wakeup()` to trigger CPU selection paths that would call `scx_bpf_select_cpu_dfl()` 
5. Monitor using `on_tick_begin()` callback to log task CPU assignments and detect crashes or unexpected CPU values
6. Detection: Look for kernel panics, invalid CPU access warnings, or tasks scheduled on non-existent CPUs in the logs
