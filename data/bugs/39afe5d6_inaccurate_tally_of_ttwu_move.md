# sched/fair: Fix inaccurate tally of ttwu_move_affine

- **Commit:** 39afe5d6fc59237ff7738bf3ede5a8856822d59d
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

Non-affine wakeups are incorrectly counted as affine wakeups by schedstats. When `wake_affine_idle()` returns `prev_cpu` (a valid CPU that is not equal to `nr_cpumask_bits`), the subsequent condition check incorrectly classifies this as an affine wakeup, causing the `ttwu_move_affine` statistic to be incremented even though the wakeup should not be counted as affine.

## Root Cause

The `wake_affine()` function checks `if (target == nr_cpumask_bits)` to determine whether an affine target was selected. However, when `wake_affine_idle()` returns `prev_cpu` (which is a valid CPU value, not `nr_cpumask_bits`), the condition evaluates to false and the code proceeds to increment affine wakeup statistics. This is incorrect because returning `prev_cpu` from `wake_affine_idle()` indicates a non-affine case, not an affine one.

## Fix Summary

The condition is changed from `if (target == nr_cpumask_bits)` to `if (target != this_cpu)`. This correctly identifies when the selected target is an affine move (i.e., when it equals `this_cpu`) and only increments the affine wakeup counters in that case. All other target values, including `prev_cpu` and `nr_cpumask_bits`, result in returning `prev_cpu` without incrementing affine statistics.

## Triggering Conditions

This bug occurs during task wakeup when the wake affine logic is enabled (WA_IDLE feature). The trigger requires:
- A task waking up on a different CPU from where it previously ran (`this_cpu != prev_cpu`)
- `wake_affine_idle()` must return `prev_cpu` instead of `nr_cpumask_bits`, which happens when `prev_cpu` is idle but `this_cpu` is not idle, or when sync wakeup conditions favor staying on `prev_cpu`
- The schedstats feature must be enabled to observe incorrect accounting
- The bug manifests as inflated `ttwu_move_affine` and `nr_wakeups_affine` counters in `/proc/schedstat`

## Reproduce Strategy (kSTEP)

Use a 3-CPU setup (CPU 0 reserved for driver, test on CPUs 1-2). Create two tasks:
1. In `setup()`: Call `kstep_task_create()` twice for waker and wakee tasks
2. In `run()`: Pin waker to CPU 1, pin wakee to CPU 2 using `kstep_task_pin()`
3. Wake both tasks with `kstep_task_wakeup()`, run for several ticks with `kstep_tick_repeat(10)`
4. Pause the wakee on CPU 2 with `kstep_task_pause()` to make CPU 2 idle
5. From CPU 1, wake the wakee with `kstep_task_wakeup()` - this triggers the path where `wake_affine_idle()` returns `prev_cpu` (CPU 2)
6. Check schedstats via `/proc/schedstat` or trace the `ttwu_move_affine` counter increments
7. On buggy kernels, affine stats incorrectly increment even though the task stays on `prev_cpu`
