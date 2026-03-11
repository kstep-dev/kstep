# sched/deadline: Fix stale throttling on de-/boosted tasks

- **Commit:** 46fcc4b00c3cca8adb9b7c9afdd499f64e427135
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

A boosted task can become stuck in a "forever-throttled" state if it is throttled during deboost. When the task is later rebooted to the deadline scheduling class, it won't be properly enqueued because the dl_throttled flag is still set. The result is a task that appears runnable but is not on the rq, with no mechanism to clear the throttle—the task is permanently stuck.

## Root Cause

When a !SCHED_DEADLINE task is deboosted while throttled, the enqueue_task_dl function returns early without clearing the dl_throttled flag. If the task is later boosted again while sleeping, the normal wakeup path does not set ENQUEUE_REPLENISH, so the task is not actually placed on the rq. No replenishment happens and no timer is set, leaving the task stuck.

## Fix Summary

Clear the dl_throttled flag before returning in the deboosting case. This ensures that when the task is rebooted to the deadline scheduling class, it won't be stuck in a throttled state and can be properly enqueued.

## Triggering Conditions

The bug occurs in the deadline scheduler's enqueue_task_dl function when:
- A task is boosted to SCHED_DEADLINE class and gets throttled due to runtime exhaustion
- During sched-out, the task is deboosted back to its original scheduling class (e.g., SCHED_NORMAL)
- The deboosting happens while the task is still in dl_throttled=1 state
- Later, the sleeping task gets boosted to SCHED_DEADLINE again
- Upon wakeup, enqueue_task_dl is called without ENQUEUE_REPLENISH flag
- The task has dl_throttled=1 but no replenishment occurs, leaving it stuck off the runqueue
- Requires priority inheritance scenarios where deadline tasks boost normal tasks

## Reproduce Strategy (kSTEP)

Create a 2-CPU setup to reproduce the deadline throttling bug:
- Use kstep_task_create() to create a normal SCHED_NORMAL task
- Use kstep_task_create() to create a SCHED_DEADLINE task with tight deadline
- Set up priority inheritance by having the deadline task block on a mutex held by the normal task
- Use kstep_tick_repeat() to run until the boosted normal task gets throttled during execution
- Use kstep_task_pause() to trigger deboosting while the normal task is throttled
- Use kstep_task_wakeup() to wake the normal task, then boost it again via mutex contention
- Monitor task state via on_tick_begin callback to check if task is runnable but not on rq
- Check p->dl.dl_throttled and p->on_rq flags to detect the stuck state
- Log when task appears runnable (p->state == TASK_RUNNING) but p->on_rq == 0
