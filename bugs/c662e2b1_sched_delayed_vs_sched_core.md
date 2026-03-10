# sched: Fix sched_delayed vs sched_core

- **Commit:** c662e2b1e8cfc3b6329704dab06051f8c3ec2993
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When a task is marked with the `sched_delayed` flag (indicating it should be dequeued later), it may still be processed by the sched_core enqueue and dequeue functions. This leads to a double dequeue situation where the task is dequeued twice—once by the sched_delayed logic and once by the sched_core logic—causing scheduling inconsistencies and potential data corruption in the core scheduling tree.

## Root Cause

The sched_core_enqueue() and sched_core_dequeue() functions were not checking for the `sched_delayed` flag before processing tasks. When the delayed dequeue feature was introduced in commit 152e11f6df29, these core scheduler functions were not updated to account for tasks that should defer their dequeue operation, resulting in tasks being processed by both the delayed dequeue mechanism and the sched_core logic simultaneously.

## Fix Summary

The fix adds early return checks in both sched_core_enqueue() and sched_core_dequeue() to skip processing if the task has the sched_delayed flag set. This ensures that tasks deferring their dequeue operation are not also processed by sched_core logic, preventing the double dequeue issue.

## Triggering Conditions

This bug requires core scheduling to be enabled with tasks having core cookies. The race occurs when:
- A task with a core cookie becomes eligible for delayed dequeue (se.sched_delayed flag set)
- The task undergoes a scheduling event that triggers sched_core_enqueue() or sched_core_dequeue()
- Without the fix, these functions proceed to modify the core_tree despite the task being marked for delayed processing
- This creates a window where the task exists in both the delayed dequeue state and the sched_core tree
- The subsequent actual dequeue operation attempts to remove the task from core_tree again, causing double dequeue
- Timing-dependent: requires delayed dequeue logic and core scheduling operations to intersect

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs beyond CPU 0. Create tasks with core cookies to enable sched_core processing:
- setup(): Create multiple CFS tasks using kstep_task_create(), assign different core cookies
- Use kstep_cgroup_create() to set up core scheduling groups if needed
- run(): Force conditions that trigger delayed dequeue on tasks with core cookies
- Use kstep_task_pause() followed by kstep_task_wakeup() to trigger enqueue/dequeue cycles
- Create scheduling pressure with kstep_tick_repeat() to force delayed dequeue scenarios
- Add logging in on_tick_begin() or on_sched_softirq_end() callbacks to monitor core_tree state
- Check task->se.sched_delayed flag and core_tree consistency during scheduling events
- Look for tasks appearing in core_tree while having sched_delayed set, or double removal errors
- Detect bug by monitoring for inconsistent core scheduling tree state or panics during dequeue operations
