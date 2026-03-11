# sched_ext: Fix SCX_TASK_INIT -> SCX_TASK_READY transitions in scx_ops_enable()

- **Commit:** 9753358a6a2b011478e8efdabbb489216252426f
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (Extensible Scheduler Class)

## Bug Description

In scx_ops_enable(), tasks were left in SCX_TASK_INIT state after the first loop called scx_ops_init_task(), then transitioned directly to SCX_TASK_ENABLED in the second loop. This invalid state transition (INIT -> ENABLED) caused a kernel warning and crashed when a task was toggled between SCHED_OTHER and SCHED_SCX. The INIT state should only be used during fork; tasks should transition to READY after initialization.

## Root Cause

The first task iteration loop in scx_ops_enable() did not transition tasks to SCX_TASK_READY after calling scx_ops_init_task(). The second loop then attempted to switch these INIT-state tasks directly into SCX, violating the state machine that requires READY as an intermediate state before ENABLED. This inconsistency with the fork path semantics caused invalid state transition warnings.

## Fix Summary

The fix moves scx_set_task_state(p, SCX_TASK_READY) from the second loop into the first loop, immediately after scx_ops_init_task() succeeds. This ensures tasks properly transition from INIT to READY, then from READY to ENABLED, maintaining consistent state machine semantics.

## Triggering Conditions

The bug is triggered in the sched_ext subsystem during scx_ops_enable() when enabling a BPF scheduler. It requires a race condition where:
- A sched_ext BPF scheduler is being loaded/enabled (calling scx_ops_enable())
- Concurrently, a task is being switched from SCHED_OTHER to SCHED_SCX via sched_setscheduler()
- The task hits the invalid state transition path in scx_ops_enable_task() where it tries to transition directly from SCX_TASK_INIT (1) to SCX_TASK_ENABLED (3)
- This violates the state machine that requires SCX_TASK_READY (2) as intermediate state
- The timing window exists between the two loops in scx_ops_enable(): first loop calls scx_ops_init_task() leaving tasks in INIT state, second loop enables them

## Reproduce Strategy (kSTEP)

This bug requires sched_ext functionality which may not be directly supported by current kSTEP. If sched_ext support were available:
- Use 2+ CPUs (CPU 0 reserved for driver)
- In setup(): Create multiple tasks with kstep_task_create(), prepare a simple sched_ext BPF program
- In run(): Start background task that repeatedly calls sched_setscheduler() to toggle between SCHED_OTHER and SCHED_SCX
- Concurrently trigger scx_ops_enable() to load the BPF scheduler (would need kstep_scx_enable() API)
- Use on_tick_begin() callback to monitor task scheduler policy changes and detect state transition warnings
- Check for "Invalid task state transition 1 -> 3" kernel warnings in dmesg or via kstep logging
- Success condition: Reproduce the warning/crash during the race between scheduler policy changes and sched_ext enabling
