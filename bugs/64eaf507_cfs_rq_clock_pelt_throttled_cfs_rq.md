# Fix cfs_rq_clock_pelt() for throttled cfs_rq

- **Commit:** 64eaf50731ac0a8c76ce2fedd50ef6652aabc5ff
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/pelt.h, kernel/sched/sched.h
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

After commit 23127296889f switched PELT (Per-Entity Load Tracking) calculations from `rq_clock_task()` to `rq_clock_pelt()`, the throttled CFS runqueue accounting was not updated accordingly. This caused `cfs_rq_clock_pelt()` to calculate the PELT clock incorrectly for throttled cfs_rqs by mixing values from the old `rq_clock_task()` with the new `rq_clock_pelt()` clock domain. The inconsistency would result in incorrect load tracking calculations for throttled task groups, potentially leading to suboptimal scheduling decisions.

## Root Cause

The previous commit updated the main PELT clock calculations to use `rq_clock_pelt()` but overlooked updating the throttled cfs_rq accounting fields (`throttled_clock_task` and `throttled_clock_task_time`). These fields were still being set and used with `rq_clock_task()` values, creating a clock domain mismatch. When `cfs_rq_clock_pelt()` tried to compute the PELT time for a throttled cfs_rq, it mixed incompatible clock values, producing incorrect results.

## Fix Summary

The fix renames `throttled_clock_task` to `throttled_clock_pelt` and `throttled_clock_task_time` to `throttled_clock_pelt_time`, and updates all related accounting code to use `rq_clock_pelt(rq)` instead of `rq_clock_task(rq)`. This ensures that PELT clock calculations for throttled cfs_rqs use consistent clock values throughout.

## Triggering Conditions

The bug occurs when CFS bandwidth control is enabled and a task group becomes throttled. Specifically:
- A cgroup with CFS bandwidth limits (`cpu.cfs_period_us` and `cpu.cfs_quota_us`) must be configured
- Tasks in the cgroup must consume their allocated bandwidth quota, triggering throttling via `tg_throttle_down()`
- PELT calculations must be performed on the throttled cfs_rq through `cfs_rq_clock_pelt()`
- The clock domain mismatch manifests when `cfs_rq_clock_pelt()` is called, mixing `rq_clock_pelt()` values with the incorrectly stored `throttled_clock_task` (saved with `rq_clock_task()`)
- This creates inconsistent PELT time calculations, affecting load tracking accuracy for throttled task groups

## Reproduce Strategy (kSTEP)

To reproduce this bug using kSTEP framework:
- Setup: Use at least 2 CPUs (CPU 0 reserved for driver). Create a cgroup with tight bandwidth limits using `kstep_cgroup_create()` and `kstep_cgroup_write()` to set `cpu.cfs_period_us=10000` and `cpu.cfs_quota_us=5000` (50% bandwidth)
- Create multiple CPU-intensive tasks with `kstep_task_create()` and add them to the cgroup using `kstep_cgroup_add_task()`
- Run tasks with `kstep_task_wakeup()` and let them consume bandwidth via `kstep_tick_repeat()`
- Monitor throttling state by checking `cfs_rq->throttle_count > 0` in `on_tick_end()` callback
- When throttled, directly call `cfs_rq_clock_pelt()` and compare results between buggy/fixed kernels
- Bug detection: Log the clock values from `throttled_clock_task`/`throttled_clock_pelt` and verify clock domain consistency
- Expected: On buggy kernel, mixed clock domains cause incorrect PELT time calculations; fixed kernel shows consistent values
