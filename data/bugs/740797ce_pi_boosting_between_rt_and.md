# Fix PI boosting between RT and DEADLINE tasks

- **Commit:** 740797ce3a124b7dd22b7fb832d87bc8fba1cf6f
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** Deadline, core

## Bug Description

When an RT task blocks on an rt_mutex held by a DEADLINE task, the priority inheritance (PI) boosting code incorrectly marks the mutex holder as dl_boosted even if the RT task is not running at DEADLINE priority. This causes a kernel warning at setup_new_dl_entity() when the boosted task tries to enqueue, because setup_new_dl_entity() expects that boosted tasks should never have an uninitialized (past) deadline.

## Root Cause

The boosting condition in rt_mutex_setprio() compares the pi_task's dynamic deadline with the current task's deadline without first verifying that the pi_task is actually running at DEADLINE priority. Since RT tasks have default/uninitialized dynamic deadline values of 0, the dl_entity_preempt() comparison incorrectly succeeds, causing the mutex holder to be marked as boosted when it shouldn't be.

## Fix Summary

Add a check `dl_prio(pi_task->prio)` before using the pi_task's dynamic deadline value in the boosting condition. This ensures the pi_task is actually running at DEADLINE priority before comparing deadlines, preventing RT tasks from triggering incorrect deadline boosting behavior.

## Triggering Conditions

This bug occurs in the priority inheritance (PI) boosting path in `rt_mutex_setprio()` when:
- An RT task blocks on an rt_mutex held by a DEADLINE task
- The PI boosting logic tries to boost the mutex holder (DEADLINE task) 
- The RT task (pi_task) has an uninitialized deadline value (typically 0)
- The boosting condition `dl_entity_preempt(&pi_task->dl, &p->dl)` incorrectly succeeds because RT tasks have deadline=0 which appears "earlier" than any valid DEADLINE task deadline
- When the boosted DEADLINE task later tries to enqueue, `setup_new_dl_entity()` is called and triggers `WARN_ON(dl_se->dl_boosted)` because boosted tasks shouldn't have past deadlines

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create an RT task and a DEADLINE task with rt_mutex contention:
- Setup: Create one RT task (`kstep_task_create()` + RT prio) and one DEADLINE task (requires kernel thread or manual dl_se setup since kSTEP lacks direct DEADLINE task creation)
- In `run()`: Have the DEADLINE task acquire an rt_mutex, then block the RT task on the same mutex using `kstep_task_pause()` and mutex synchronization
- Use `on_tick_begin()` callback to monitor task states and log when PI boosting occurs
- Trigger enqueue of the boosted DEADLINE task via `kstep_task_wakeup()` or CPU migration
- Detect bug by checking for kernel warnings in dmesg or by monitoring `dl_se->dl_boosted` flag during enqueue operations
- On buggy kernels: expect WARN_ON at `setup_new_dl_entity()` when boosted DEADLINE task enqueues
