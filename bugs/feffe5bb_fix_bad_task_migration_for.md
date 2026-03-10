# sched/rt: Fix bad task migration for rt tasks

- **Commit:** feffe5bb274dd3442080ef0e4053746091878799
- **Affected file(s):** kernel/sched/rt.c, kernel/sched/deadline.c
- **Subsystem:** RT, Deadline

## Bug Description

A race condition allows a task with `migrate_disable()` set to be migrated anyway. The issue occurs when pushing RT/deadline tasks: after the initial migration_disabled check passes, the task can be woken up on another CPU, call migrate_disable() and get preempted, all before the migration code re-acquires locks. When it retries the migration, it fails to detect that migration is now disabled and proceeds with the migration, triggering a WARN_ON_ONCE in set_task_cpu().

## Root Cause

The check for `is_migration_disabled()` happens before acquiring the destination runqueue lock in find_lock_lowest_rq() and find_lock_later_rq(). Between this check and the double_lock_balance() call that acquires the locks, a race window exists where another CPU can wake the task, enable migrate_disable, and cause a preemption. When double_lock_balance() retries after unlocking and re-locking, the code does not re-verify the migration_disabled status, allowing migration of a task that should be pinned.

## Fix Summary

The fix adds an `is_migration_disabled(task)` check in the retry logic after double_lock_balance() in both find_lock_lowest_rq() (RT scheduler) and find_lock_later_rq() (deadline scheduler). This ensures that if the task became migration-disabled during the lock acquisition race, the migration attempt is aborted and the function returns NULL to retry or skip migration.

## Triggering Conditions

The bug requires a precise race between RT/deadline task migration and migrate_disable():
- Two CPUs where CPU0 has high-priority RT/deadline task triggering push_rt_task/push_dl_task
- Target task initially has migration_disabled == 0 and is not running 
- CPU1 must wake the target task, call migrate_disable(), then preempt it during CPU0's double_lock_balance()
- The timing window occurs after the initial is_migration_disabled() check passes but before locks are re-acquired
- Task must remain on original runqueue but with migration now disabled when migration code retries
- Results in WARN_ON_ONCE() in set_task_cpu() when attempting to migrate a migration-disabled task

## Reproduce Strategy (kSTEP)

Configure 2+ CPUs and create RT tasks to trigger the push migration path:
1. Use kstep_task_create() to create 3 RT tasks: pusher, target, and disabler
2. Pin pusher to CPU1 with kstep_task_pin() and set RT priority with kstep_task_fifo()
3. Create target task on CPU2 initially without migration disable, also RT priority
4. Use kstep_task_wakeup() to wake pusher, triggering push_rt_task() when CPU1 becomes overloaded
5. In on_tick_begin() callback, detect when push_rt_task() begins migration of target task
6. Synchronously wake target task on CPU2 with kstep_task_wakeup(), then immediately call migrate_disable() simulation
7. Use kstep_tick() to advance scheduler state during the double_lock_balance() window
8. Monitor for WARN_ON_ONCE() in kernel logs or use kstep_freeze_task() to check migration_disabled flag
9. Success: WARN_ON_ONCE triggered. Failure: migration correctly aborted without warning
