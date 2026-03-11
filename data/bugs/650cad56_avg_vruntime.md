# sched/eevdf: Fix avg_vruntime()

- **Commit:** 650cad561cce04b62a8c8e0446b685ef171bc3bb
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (eEVDF)

## Bug Description

The `avg_vruntime()` function fails to ensure task eligibility in a corner case involving negative averages. When the average vruntime is negative relative to min_vruntime, tasks placed at the returned average value are incorrectly marked as ineligible, violating the scheduler's fundamental guarantee that a task positioned at avg_vruntime() should be eligible for execution.

## Root Cause

The bug occurs due to how integer division behaves with signed numbers in C. When dividing positive numbers, integer division floors (discards remainder), producing a result slightly left of the true average. However, when the dividend is negative, the same division operation ceils (rounds toward zero), producing a result right of the true average. This behavior flip causes `avg_vruntime()` to return a value just right of center when the average is negative, placing the task outside the eligibility range.

## Fix Summary

The fix adds a conditional correction: when `avg < 0`, it subtracts `(load - 1)` from `avg` before division to restore the left bias property. This ensures the division operation consistently produces a value on the correct side of the true average regardless of whether the average is positive or negative, maintaining the eligibility invariant.

## Triggering Conditions

The bug requires a negative weighted average in the CFS runqueue's `avg_vruntime` calculation:
- Tasks with different nice values/weights to ensure avg/load is not evenly divisible
- A task with positive virtual lag (vlag) from previous execution gets paused and later awakened
- The `place_entity()` code path during task wakeup calls `avg_vruntime()` with a negative average
- Current task and sleeping task states create avg < 0 relative to min_vruntime
- Integer division of negative avg by positive load ceils instead of floors, placing the task just right of true average
- This violates the eligibility invariant that tasks at avg_vruntime() should be eligible

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (driver uses CPU 0). Create 3 tasks with different weights:
- In `setup()`: Use `kstep_task_create()` to create tasks A, B, C
- In `run()`: Set different priorities with `kstep_task_set_prio()` (e.g., A=nice 1, B=nice 0)
- Wake tasks A and B with `kstep_task_wakeup()`, run with `kstep_tick_repeat(10)` to build vruntime differences
- Pause task B using `kstep_task_pause()` when it has positive vlag, tick to process dequeue
- Run task A alone with `kstep_tick_repeat(10)` to advance min_vruntime past B's saved position
- Wake task B with `kstep_task_wakeup()` to trigger `place_entity()` with negative key
- Wake task C (vlag=0) to test placement at `avg_vruntime()` - should be eligible but will fail in buggy kernel
- Use `kstep_eligible()` to verify C's eligibility status and detect the bug
