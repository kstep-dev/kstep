# sched/fair: Properly deactivate sched_delayed task upon class change

- **Commit:** 75b6499024a6c1a4ef0288f280534a5c54269076
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair Scheduling)

## Bug Description

When a task is switched away from the fair scheduling class (via `__sched_setscheduler()`), the task remains on_rq (in the runqueue) despite being deactivated. The dequeue sequence during class change fails to call `__block_task()`, which is the final step that properly removes a delayed-dequeue task from the runqueue. This leaves the task in an inconsistent state—marked as dequeued but still physically on the runqueue.

## Root Cause

The `switched_from_fair()` function handles the transition when a task changes out of the fair class. It calls `dequeue_task(rq, p, DEQUEUE_NOCLOCK | DEQUEUE_SLEEP)` to deactivate the task, but cannot use the `DEQUEUE_DELAYED` flag (which is fair-class-specific). The normal dequeue path for fair tasks calls `__block_task()` at the end of `dequeue_entities()`, but since `__sched_setscheduler()` doesn't use `DEQUEUE_DELAYED`, this crucial cleanup step is skipped, leaving the task on the runqueue.

## Fix Summary

The fix extracts the delayed-dequeue cleanup logic into a helper function `finish_delayed_dequeue_entity()` and adds an explicit call to `__block_task(rq, p)` in `switched_from_fair()` to properly complete the deactivation of sched_delayed tasks upon class changes. This ensures the task is fully removed from the runqueue when switching scheduling classes.

## Triggering Conditions

This bug occurs when a CFS task with `sched_delayed=1` undergoes a scheduling class change via `__sched_setscheduler()`. The task must first be in the fair scheduling class and have its `se.sched_delayed` flag set to 1, which happens when the task is dequeued with `DEQUEUE_DELAYED` (typically during sleep/pause operations). When `__sched_setscheduler()` changes the task to a different scheduling class (RT, DL, etc.), `switched_from_fair()` dequeues the task using `DEQUEUE_NOCLOCK | DEQUEUE_SLEEP` flags, but cannot use `DEQUEUE_DELAYED` since it's fair-class-specific. This leaves the task in an inconsistent state: `se.sched_delayed=1`, but still `on_rq=1` despite being logically dequeued.

## Reproduce Strategy (kSTEP)

Create a CFS task and trigger the sched_delayed state, then force a class change to expose the bug:
1. Use `kstep_task_create()` to create a CFS task and `kstep_task_wakeup()` to enqueue it
2. Run the task briefly with `kstep_tick_repeat(5)` to establish runtime state
3. Call `kstep_task_pause()` to trigger sched_delayed dequeue (sets `se.sched_delayed=1`)  
4. Use `kstep_tick()` to process the pause and verify task is dequeued but `sched_delayed=1`
5. Trigger class change by setting RT priority with `kstep_task_set_prio(task, -10)` (RT priority)
6. Check `task->se.on_rq` in `on_tick_end()` callback - it should be 0 after class change but will be 1 in buggy kernel
7. Log the task state with `TRACE_INFO("BUG: task on_rq=%d sched_delayed=%d after class change", task->se.on_rq, task->se.sched_delayed)` to detect the inconsistency
