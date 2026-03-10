# sched/eevdf: Fixup PELT vs DELAYED_DEQUEUE

- **Commit:** fc1892becd5672f52329a75c73117b60ac7841b7
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS (Fair Scheduling with EEVDF)

## Bug Description

Tasks placed in delayed-dequeue state (kept on the runqueue to burn off negative lag) were being incorrectly counted as runnable in PELT load tracking metrics. Since these tasks are not actually runnable and will be dequeued the moment they are picked for execution, including them in load tracking produces incorrect scheduling decisions and inaccurate runnable state information.

## Root Cause

The DELAYED_DEQUEUE feature keeps ineligible tasks on the runqueue temporarily without updating load metrics when transitioning to this state. Additionally, the `se_runnable()` function did not check the `sched_delayed` flag, causing these tasks to be incorrectly included in runnable state calculations even though they should be excluded.

## Fix Summary

The fix adds `update_load_avg()` calls at the boundary points where tasks transition to and from delayed-dequeue state (in `dequeue_entity()` and `requeue_delayed_entity()`) to properly account for this state change in PELT metrics. It also modifies `se_runnable()` to explicitly exclude delayed-dequeue tasks, ensuring they are not counted as runnable.

## Triggering Conditions

The bug occurs in the CFS scheduler when the DELAY_DEQUEUE feature is enabled and tasks with negative lag are dequeued for sleep. The key conditions are:
- A task accumulates negative lag (vruntime below the cfs_rq's min_vruntime)
- The task transitions to sleep state via `dequeue_entity()` with `DEQUEUE_SLEEP` flag
- The task is ineligible (`!entity_eligible()`) due to its negative lag
- DELAY_DEQUEUE feature places the task in delayed-dequeue state instead of removing it
- PELT load tracking continues counting the delayed task as runnable despite `sched_delayed=1`
- Load balancing and scheduling decisions become incorrect due to inflated runnable counts

## Reproduce Strategy (kSTEP)

Create two tasks with different nice values to generate uneven vruntime progression. Use at least 2 CPUs (driver uses CPU 0):
1. In `setup()`: Create tasks A (nice +1, lower weight) and B (nice 0, higher weight) with `kstep_task_create()`
2. Set priorities: `kstep_task_set_prio(task_a, 1)` for unequal weights
3. Wake both tasks and run: `kstep_task_wakeup()` then `kstep_tick_repeat(20)` 
4. Pause task B to trigger `dequeue_entity()`: `kstep_task_pause(task_b)`, `kstep_tick()`
5. Run task A alone to advance min_vruntime: `kstep_tick_repeat(15)`
6. Detect bug: Check if `se_runnable(&task_b->se)` incorrectly returns true while `task_b->se.sched_delayed=1`
7. Use `on_tick_begin` callback to log runnable counts and compare with actual task states
8. Verify delayed tasks inflate `cfs_rq->avg_load` without corresponding runnable behavior
