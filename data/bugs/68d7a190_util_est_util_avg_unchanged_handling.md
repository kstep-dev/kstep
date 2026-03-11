# sched/fair: Fix util_est UTIL_AVG_UNCHANGED handling

- **Commit:** 68d7a190682aa4eb02db477328088ebad15acc83
- **Affected file(s):** kernel/sched/debug.c, kernel/sched/fair.c, kernel/sched/pelt.h
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The UTIL_AVG_UNCHANGED flag was stored in the LSB of util_est.enqueued and exported through _task_util_est(), causing task_util_est() to never return 0 even when utilization was zero. This prevented find_energy_efficient_cpu() from returning prev_cpu early as intended. Additionally, in util_est_update(), the UTIL_AVG_UNCHANGED flag was inconsistently applied—only masking the subtrahend but not the minuend, leading to incorrect utilization estimates.

## Root Cause

The LSB-based flag design exposed the internal flag through public APIs, corrupting the returned utility value. When the max of ewma and enqueued was ORed with UTIL_AVG_UNCHANGED, the flag bit became part of the utility value itself, making zero-checks fail. The asymmetric flag usage in util_est_update() (only on one side of the equation) caused the calculation to be inconsistent and incorrect.

## Fix Summary

The fix moves UTIL_AVG_UNCHANGED from the LSB to the MSB of util_est.enqueued, keeping the flag internal and invisible to public APIs. _task_util_est() now masks off the flag before returning, ensuring utility values are not corrupted. The flag is set at the end of util_est_update(), maintaining consistent semantics throughout the update logic.

## Triggering Conditions

This bug manifests in the CFS scheduler's util_est (utilization estimation) subsystem when:
- SCHED_FEAT(UTIL_EST, true) is enabled (default setting)
- A task has zero actual utilization but util_est.enqueued contains the UTIL_AVG_UNCHANGED LSB flag
- find_energy_efficient_cpu() calls task_util_est() expecting it to return 0 for zero-util tasks
- The UTIL_AVG_UNCHANGED flag (value 1) in the LSB makes task_util_est() return 1 instead of 0
- In util_est_update(), asymmetric flag application creates calculation inconsistencies
- Energy-aware scheduling decisions are corrupted as prev_cpu early return path fails
- Tasks undergo enqueue/dequeue cycles that trigger util_est updates with the flag set

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). Create a task that alternates between running and sleeping to trigger util_est updates. In setup(), use kstep_task_create() to create a test task. In run(), use kstep_task_wakeup() followed by kstep_tick_repeat() to let the task accumulate some utilization, then kstep_task_pause() to trigger dequeue and util_est storage. Call kstep_sleep() or kstep_tick_repeat() extensively to let utilization decay to near-zero. Wake the task again with kstep_task_wakeup() and examine util_est values. Monitor task_util_est() return values through logging - the bug causes it to return 1 (UTIL_AVG_UNCHANGED) instead of 0 for zero-util tasks. Use on_tick_begin() callback to periodically log task utilization state. Check that find_energy_efficient_cpu() path optimization fails due to non-zero task_util_est().
