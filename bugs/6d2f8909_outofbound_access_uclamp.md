# sched: Fix out-of-bound access in uclamp

- **Commit:** 6d2f8909a5fabb73fe2a63918117943986c39b6c
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

Util-clamp maps tasks to buckets based on their clamp values, but the bucket size calculation using rounding division can cause an off-by-one error, resulting in out-of-bounds memory access. For instance, with 20 buckets, a task with clamp value 1024 gets mapped to bucket id 20, but valid indexes are in the range [0,19].

## Root Cause

The bucket id is computed by dividing the clamp value by the bucket delta (UCLAMP_BUCKET_DELTA), which is calculated using DIV_ROUND_CLOSEST. When a clamp value equals SCHED_CAPACITY_SCALE, the division can yield a bucket id equal to or greater than UCLAMP_BUCKETS, exceeding the maximum valid index.

## Fix Summary

The fix clamps the computed bucket id to the maximum valid value (UCLAMP_BUCKETS - 1) using `min_t()`, ensuring the bucket id always remains within the valid range [0, UCLAMP_BUCKETS - 1].

## Triggering Conditions

The bug is triggered when a task has a util-clamp value equal to SCHED_CAPACITY_SCALE (1024). This occurs during task enqueue/dequeue operations that call `uclamp_bucket_id()` to map the clamp value to a bucket index. With the default configuration of UCLAMP_BUCKETS=20, UCLAMP_BUCKET_DELTA becomes 51 (DIV_ROUND_CLOSEST(1024, 20)). A task with clamp value 1024 gets mapped to bucket id 1024/51=20, but valid indexes are [0,19], causing out-of-bounds access to the uclamp bucket arrays in the CPU runqueue structure. The bug manifests when tasks modify their uclamp values via sched_setattr() or through cgroup cpu.uclamp settings, and can lead to memory corruption or system crashes.

## Reproduce Strategy (kSTEP)

Set up a single CPU system (CPU 1, since CPU 0 is reserved). In `setup()`, create a task using `kstep_task_create()` and configure util-clamp by writing to cgroup cpu.uclamp.max with value 1024 using `kstep_cgroup_create()`, `kstep_cgroup_set_weight()`, and `kstep_cgroup_write()`. In `run()`, wake the task with `kstep_task_wakeup()` to trigger enqueue path, then pause it with `kstep_task_pause()` to trigger dequeue path. Use `on_tick_begin()` callback to log bucket computations and detect out-of-bounds access. Monitor for bucket_id >= UCLAMP_BUCKETS (20) in the logs. The bug should manifest as invalid bucket access during the enqueue/dequeue operations when uclamp_bucket_id() computes bucket 20 for clamp value 1024.
