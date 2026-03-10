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
