# sched/ext: Fix invalid task state transitions on class switch

- **Commit:** ddf7233fcab6c247379d0928d46cc316ee122229
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

When enabling a sched_ext scheduler, invalid task state transitions occur for dead tasks, triggering kernel warnings. The issue is reproducible by running the hotplug selftest in a loop, resulting in warnings like "sched_ext: Invalid task state transition 0 -> 3 for fish[770]" and a WARNING at scx_set_task_state(). This causes failures during scheduler class switching when tasks with zero usage counters (dead tasks) are processed.

## Root Cause

During the sched_ext enablement process, the code skips initialization for dead tasks (those with usage counter set to zero) but does not exclude them during the scheduling class transition phase. When a dead task is passed through the class switching logic, it attempts an invalid state transition that violates scheduler constraints, triggering the warning.

## Fix Summary

The fix adds reference counting using tryget_task_struct() and put_task_struct() around the class switching loop. If a task is dead (reference count is zero), tryget_task_struct() fails and the task is skipped via continue, preventing it from entering the invalid state transition code path.

## Triggering Conditions

The bug occurs during sched_ext scheduler enablement when dead tasks (usage count = 0) are processed in the class switching phase. Specifically:
- A sched_ext scheduler is being enabled via BPF system calls
- The system contains tasks that have died but still exist in the task iterator
- During the class switching loop in scx_enable(), dead tasks bypass the tryget_task_struct() check
- These dead tasks attempt invalid state transitions (e.g., SCX_TASK_NONE -> SCX_TASK_ENABLED)
- The scx_set_task_state() function detects the invalid transition and triggers a WARNING
- Race condition occurs between task death and scheduler enablement timing

## Reproduce Strategy (kSTEP)

This bug requires sched_ext support which is not available in standard kSTEP framework. However, the reproduction approach would be:
- Use at least 2 CPUs (CPU 0 reserved for driver)
- Create multiple tasks using kstep_task_create() and make them exit rapidly
- Simulate sched_ext enablement by calling into kernel scheduler transition paths
- Set up timing where tasks die during scheduler class switching
- Monitor for invalid state transition warnings in kernel logs via TRACE_INFO()
- Use on_tick_begin callback to check task states during transitions
- Look for WARNING messages from scx_set_task_state() indicating state 0->3 transitions
- Verify fix by ensuring tryget_task_struct() properly skips dead tasks
- Note: Full reproduction requires kernel build with CONFIG_SCHED_CLASS_EXT=y
