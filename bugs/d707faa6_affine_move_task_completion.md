# sched/core: Add missing completion for affine_move_task() waiters

- **Commit:** d707faa64d03d26b529cc4aea59dab1b016d4d33
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A task calling sched_setaffinity() becomes indefinitely stuck on wait_for_completion() when a race condition occurs: the task moves to an allowed CPU between when affine_move_task() schedules the migration stopper and when the stopper actually runs. Since the stopper's rq no longer matches the task's current rq, the completion is never signaled, leaving the waiting task hung forever.

## Root Cause

In migration_cpu_stop(), when a task has migrated to a different rq, the code only checked `dest_cpu < 0` to determine whether to signal completion of pending affinity requests. If dest_cpu was >= 0 (a specific destination), the completion was skipped even when a pending request existed. A race allows the task to move to an allowed CPU while the stopper is running, creating a scenario where the completion should be signaled but isn't.

## Fix Summary

The fix adds an additional check `|| pending` to the condition and introduces logic to detect when a pending affinity request can be satisfied because the task has already moved to a CPU within its allowed mask. When this condition is met, the migration_pending request is completed, allowing the waiting task to proceed.

## Triggering Conditions

The bug occurs in sched/core's CPU affinity migration path when:
- Task A calls sched_setaffinity() while running, triggering affine_move_task() 
- affine_move_task() calls stop_one_cpu() and waits on wait_for_completion(&pending->done)
- Between scheduling the migration stopper and when it runs, task A migrates to an allowed CPU
- migration_cpu_stop() runs but task_rq(p) != rq (task moved), and dest_cpu >= 0
- The original code only checked dest_cpu < 0 to signal completion, missing the pending case
- Task A remains stuck forever waiting for completion that never comes

## Reproduce Strategy (kSTEP)

Use 3+ CPUs (CPU 0 reserved for driver). Create a task that repeatedly calls sched_setaffinity():
- In setup(): Create target task with kstep_task_create(), pin to CPU 1 initially
- In run(): Use kstep_task_wakeup() to start the task on CPU 1  
- Create a kthread that mimics sched_setaffinity() by calling affine_move_task() indirectly
- Use kstep_tick() timing to create race: schedule migration stopper, then immediately migrate task to allowed CPU
- Monitor with on_sched_softirq_end() callback to detect when migration stopper runs
- Check if task becomes stuck (no completion signal) by monitoring task state transitions
- Bug detected when task hangs waiting for completion that never arrives
