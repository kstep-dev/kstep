# sched/core: Fix size of rq::uclamp initialization

- **Commit:** dcd6dffb0a75741471297724640733fa4e958d72
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The initialization of `rq::uclamp` was clearing only `sizeof(struct uclamp_rq)` bytes instead of the entire array. Since `rq::uclamp` is an array of `UCLAMP_CNT` elements, this left all but the first element uninitialized with garbage values. This could cause incorrect CPU utilization clamp values to be used for all but the first clamp ID, leading to unpredictable scheduler behavior and potential performance issues or incorrect task clamping.

## Root Cause

The memset call in `init_uclamp()` was passing `sizeof(struct uclamp_rq)` as the size argument, which clears only a single array element. The code failed to account for the fact that `rq::uclamp` is an array of `UCLAMP_CNT` elements, not a single struct. Without multiplying the size by `UCLAMP_CNT`, the remaining array elements remained uninitialized.

## Fix Summary

The fix multiplies the sizeof calculation by `UCLAMP_CNT` to ensure the entire array is cleared during initialization: `sizeof(struct uclamp_rq) * UCLAMP_CNT`. This guarantees all CPU clamp bucket structures are properly initialized to zero at boot time.

## Triggering Conditions

This bug occurs during kernel initialization in `init_uclamp()` when the scheduler subsystem is being set up. The uninitialized memory affects the second element (`UCLAMP_MAX`) of the `rq::uclamp` array on each CPU, leaving it with garbage values. The bug manifests when utilization clamping is used with tasks that have `UCLAMP_MAX` constraints, causing incorrect bucket refcounting, wrong effective clamp values, or scheduler crashes when accessing uninitialized bucket fields. The timing is deterministic as it happens once at boot, but the garbage values are unpredictable, making the exact symptoms vary across boots.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). In `setup()`, create tasks with uclamp constraints using `kstep_cgroup_create()` and `kstep_cgroup_set_weight()` to set utilization clamps. In `run()`, use `kstep_task_wakeup()` to place clamped tasks on different CPUs and `kstep_tick_repeat()` to let them run. Use direct memory inspection via `cpu_rq(cpu)->uclamp[UCLAMP_MAX]` to check for uninitialized bucket fields (non-zero values, invalid pointers). Monitor for crashes or incorrect clamping behavior when tasks migrate between CPUs or when bucket refcounts become corrupted. Log bucket state in `on_tick_end()` callback and detect the bug through garbage values in the uninitialized `UCLAMP_MAX` buckets.
