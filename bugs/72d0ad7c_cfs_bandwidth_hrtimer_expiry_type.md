# sched/fair: Fix CFS bandwidth hrtimer expiry type

- **Commit:** 72d0ad7cb5bad265adb2014dbe46c4ccb11afaba
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

The `runtime_refresh_within()` function checks if a CFS bandwidth quota refresh is about to occur by examining the time remaining until the `period_timer` expires. However, when the timer's remaining time is negative (indicating the timer has expired or is past its expiry time), storing this value in an unsigned 64-bit integer (`u64`) causes integer underflow. The negative value wraps around to a large positive number, causing the comparison `remaining < min_expire` to fail and return false instead of true. This causes the function to incorrectly report that a quota refresh is not imminent, leading to CFS bandwidth enforcement becoming less strict and allowing cfs_rq's to be unthrottled using runtime from previous periods.

## Root Cause

The bug occurs because `hrtimer_expires_remaining()` can return negative values (represented as negative nanosecond values in `ktime_t`), but the code was storing the result in a `u64` (unsigned type). When a negative value is cast to unsigned, it causes integer underflow, resulting in a very large positive number. The subsequent comparison then fails to detect that the timer has expired or that the remaining time should be treated as negative/zero, breaking the logic that determines whether a quota refresh is imminent.

## Fix Summary

The fix changes the `remaining` variable from `u64` to `s64` (signed 64-bit) to properly handle negative time values returned by `hrtimer_expires_remaining()`. Additionally, `min_expire` is cast to `s64` in the comparison to ensure the comparison is performed as a signed operation, correctly detecting when the remaining time is less than the minimum expiration threshold.

## Triggering Conditions

The bug is triggered when `hrtimer_expires_remaining()` returns a negative value during CFS bandwidth period timer expiry handling. This occurs when:
- A CFS bandwidth-limited cgroup has active tasks approaching or exceeding their quota
- The `period_timer` has expired or is overdue (negative remaining time)
- `runtime_refresh_within()` is called to check if quota refresh is imminent
- The negative remaining time gets cast to `u64`, causing integer underflow
- The underflowed value becomes very large, making `remaining < min_expire` fail
- This allows unthrottling using runtime from previous periods, bypassing strict enforcement

The race window is narrow but reproducible under high load when timer expiry is delayed.

## Reproduce Strategy (kSTEP)

Create a CFS bandwidth-limited cgroup with short quota periods to force frequent timer expiry checks:
- Use 2+ CPUs (CPU 0 reserved for driver, CPU 1+ for workload)
- In `setup()`: Use `kstep_cgroup_create("test")` and `kstep_cgroup_set_weight("test", 100)`
- Configure tight CFS bandwidth: `kstep_cgroup_write("test", "cpu.cfs_quota_us", "1000")` and `kstep_cgroup_write("test", "cpu.cfs_period_us", "10000")`
- In `run()`: Create multiple tasks with `kstep_task_create()`, add to cgroup via `kstep_cgroup_add_task("test", task->pid)`
- Use `kstep_tick_repeat()` with high frequency to stress bandwidth enforcement and trigger timer expiry
- Monitor via custom callback checking `runtime_refresh_within()` return values during periods of high contention
- Log when timer remaining time would be negative to detect the integer underflow condition
