# Fix race in push_rt_task

- **Commit:** 690e47d1403e90b7f2366f03b52ed3304194c793
- **Affected file(s):** kernel/sched/rt.c
- **Subsystem:** RT (Real-Time Scheduling)

## Bug Description

A race condition in push_rt_task() causes kernel panics when a task is migrated and re-queued while push_rt_task holds locks. The task migrates to another CPU and gets re-queued (on_rq=1), but the original CPU attempting to push the task fails to detect this migration. This leads to the task being dequeued from the wrong runqueue, resulting in crashes such as BUG_ON failures, NULL pointer dereferences, and queue corruption in the scheduler.

## Root Cause

The bug is a race condition in find_lock_lowest_rq(). When double_lock_balance() unlocks and reacquires the runqueue locks, another CPU (via try_to_wake_up) can dequeue the task from the original CPU and re-queue it elsewhere. The original validation checks (on_rq, task_on_cpu, rt_task, etc.) do not catch this subtle case where the task has on_rq=1 again after being migrated. The code reads task->cpu and on_rq, but between reading these and acting on them, the task may have moved to a different runqueue.

## Fix Summary

The fix verifies after acquiring locks that the task is still at the head of the pushable tasks list by calling pick_next_pushable_task(). This ensures the task hasn't been migrated and re-queued elsewhere. If the task is no longer at the head of the list, it abandons the migration attempt by returning NULL, preventing the race condition from causing corruption.

## Triggering Conditions

The bug requires the RT scheduler's push_rt_task() code path where CPU A attempts to migrate an RT task to CPU X. The race occurs when double_lock_balance() in find_lock_lowest_rq() drops and reacquires locks, creating a window where: (1) the target task gets dequeued and executed on its original CPU Z, (2) the task yields and gets dequeued (on_rq=0), (3) another CPU B calls try_to_wake_up() and re-queues the task on CPU Y (on_rq=1 again), and (4) CPU A reacquires locks and incorrectly concludes the task is still pushable because on_rq=1 and task_cpu still matches, but the task is no longer at the head of the pushable tasks list. This requires multiple CPUs with RT tasks and timing where lock contention forces double_lock_balance() to drop locks.

## Reproduce Strategy (kSTEP)

Use 4+ CPUs with CPU 0 reserved for driver. In setup(), create 4-6 RT tasks using kstep_task_create() and kstep_task_fifo(). Pin tasks to create load imbalance: multiple tasks on CPU 1, single tasks on CPUs 2-3. In run(), use kstep_task_wakeup() to queue tasks, then kstep_tick_repeat() to trigger RT load balancing. Create timing pressure by having some tasks yield via kstep_task_pause() and kstep_task_wakeup() in rapid succession while others remain runnable. Use on_sched_softirq_begin/end callbacks to log push_rt_task() attempts and task states. Monitor for BUG_ON crashes, NULL pointer dereferences, or queue corruption by checking kernel logs. The bug manifests when a task gets dequeued from the wrong runqueue, causing scheduler invariant violations.
