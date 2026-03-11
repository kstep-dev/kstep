# sched/pelt: Fix task util_est update filtering

- **Commit:** b89997aa88f0b07d8a6414c908af75062103b8c9
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (util_est)

## Bug Description

The util_est update filtering uses a 1% margin to skip updates when the EWMA signal is close to the task's util_avg. However, when a task's utilization suddenly ramps up, the filtering can skip the update even when the enqueued field is significantly different from the new util_avg. This leaves ue.enqueued with a stale value, which trace probes can observe and return incorrect data. The bug was identified through LISA's UtilConvergence testing on ARM boards.

## Root Cause

The filtering logic only checks whether the EWMA is within ~1% margin of the new util_avg. Due to EWMA decay from a previous high utilization value, EWMA can be close enough to trigger the filter, causing the update to be skipped entirely. This skips the update of ue.enqueued even though it may be significantly different from the new value, leaving it stale. The original code did not check whether the enqueued field itself needed updating before deciding to skip the entire util_est update.

## Fix Summary

The fix adds an additional check to the filtering logic that considers both the EWMA difference and the enqueued field difference from the new value. A second variable (last_enqueued_diff) is introduced to track how much the enqueued field has changed. The update is now skipped only if BOTH the EWMA and enqueued fields are within the 1% margin; if enqueued is outside the margin, the update proceeds even if EWMA is within the margin. This ensures ue.enqueued always has an up-to-date value.

## Triggering Conditions

The bug occurs in the CFS util_est update path during task dequeue operations. It requires:
- A task with previous high utilization where EWMA has decayed close to new util_avg
- A sudden utilization ramp-up causing significant difference between old and new ue.enqueued
- The EWMA difference being within ~1% margin (SCHED_CAPACITY_SCALE/100) of util_avg
- The enqueued field difference being outside the 1% margin but update gets skipped anyway
- UTIL_EST sched feature enabled and task sleeping (not just context switching)

The race occurs when util_est_update() filtering logic only checks EWMA proximity, missing cases where ue.enqueued needs updating despite EWMA being close to the target value.

## Reproduce Strategy (kSTEP)

Reproduce by creating util_avg patterns that trigger the filtering bug:
- Use 2+ CPUs (CPU 0 reserved for driver)
- Create task with kstep_task_create(), run it intensively to build high utilization
- Use kstep_task_pause() to dequeue and let EWMA decay toward current util_avg
- Create sudden ramp-up by changing task behavior and calling kstep_task_wakeup()
- Monitor via custom logging in util_est_update() to track ue.enqueued vs ue.ewma values
- Use on_tick_end() callback to log task util_est state and detect stale ue.enqueued
- Trigger multiple dequeue/enqueue cycles with kstep_task_usleep() and varying workloads
- Check for cases where ue.enqueued doesn't update despite significant difference from new value
