# sched/eevdf: Fix min_vruntime vs avg_vruntime

- **Commit:** 79f3f9bedd149ea438aaeb0fb6a083637affe205
- **Affected file(s):** kernel/sched/debug.c, kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS/EEVDF (Earliest Eligible Virtual Deadline First)

## Bug Description

The EEVDF scheduler uses `min_vruntime` as a reference point (v0) for computing the weighted average virtual runtime. However, `min_vruntime` is monotonically increasing and only moves forward, while the correct average can move backward when non-eligible tasks leave the runqueue. When these values drift far enough apart, the scheduling calculations become incorrect, breaking the fairness guarantees of the EEVDF algorithm.

## Root Cause

The bug stems from using `min_vruntime` (which only increases) as the reference point for vruntime calculations. Since the true reference point should be the weighted average of entity vruntimes (which can move backward), the divergence between `min_vruntime` and `avg_vruntime` causes entity eligibility and scheduling decisions to be computed relative to an incorrect reference point.

## Fix Summary

Replace `cfs_rq::min_vruntime` with `cfs_rq::zero_vruntime` that actively tracks the average vruntime instead of being monotonically increasing. The new `update_zero_vruntime()` function is called at enqueue and dequeue points to keep `zero_vruntime` synchronized with `avg_vruntime`, ensuring the reference point remains correct and prevents drift-related scheduling errors.

## Triggering Conditions

The bug is triggered in the EEVDF scheduler when `min_vruntime` (used as reference point v0) diverges significantly from the true weighted average vruntime. This happens when non-eligible tasks (with large positive lag/low vruntime) leave the runqueue, causing `avg_vruntime` to move backward while `min_vruntime` can only move forward. The drift accumulates over time through cycles of:
- Tasks with different weights running and accumulating different vruntime rates
- Tasks getting paused/dequeued when they have positive lag (vruntime below average)
- `min_vruntime` advancing based on current/leftmost task while average moves backward
- Entity eligibility calculations using incorrect reference point
- Task placement at wake-up using stale `avg_vruntime()` computation

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved). Create tasks with different weights to ensure uneven vruntime accumulation. In `setup()`: use `kstep_task_create()` for 3 tasks, `kstep_task_set_prio()` for different nice values. In `run()`: wake all tasks with `kstep_task_wakeup()`, run with `kstep_tick_repeat()` to build vruntime differences, pause the task with lower vruntime (positive lag) using `kstep_task_pause()`, continue running to advance `min_vruntime`, then wake the paused task. Use `on_tick_end()` callback to log `cfs_rq->min_vruntime`, `avg_vruntime(cfs_rq)`, and entity keys. Detect bug by checking if newly placed tasks have incorrect eligibility due to min_vruntime/avg_vruntime divergence exceeding threshold (~1ms of vruntime).
