# sched/fair: Fix reordering of enqueue/dequeue_task_fair()

- **Commit:** 5ab297bab984310267734dfbcc8104566658ebef
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When a cgroup is throttled, the group scheduler entity can still be enqueued with on_rq=true. When a task is enqueued on such a child cgroup, the load_avg and h_nr_running of the throttled parent cfs_rq must still be updated. However, the throttled check was happening before these updates, causing the loop to exit early via goto without performing the necessary updates to load_avg and h_nr_running.

## Root Cause

The second for_each_sched_entity() loop in enqueue_task_fair() and dequeue_task_fair() was checking for throttled cfs_rq at the beginning of the loop iteration, before calling update_load_avg() and updating h_nr_running. This meant that when the cfs_rq was throttled, the critical update operations were skipped entirely, leaving the parent cfs_rq in an inconsistent state with stale load averages and incorrect h_nr_running counts.

## Fix Summary

The fix reorders the operations in the second for_each_sched_entity() loop to perform update_load_avg() and h_nr_running updates before checking if the cfs_rq is throttled. This ensures that necessary accounting updates happen before exiting the loop, maintaining consistency even when the parent cgroup is throttled.

## Triggering Conditions

The bug requires a nested cgroup hierarchy where:
- A parent cgroup becomes throttled (exhausted CPU quota) but its group scheduling entity remains enqueued (on_rq=true)
- A task is enqueued/dequeued in a child cgroup of the throttled parent
- The second for_each_sched_entity() loop in enqueue_task_fair()/dequeue_task_fair() traverses from child to parent
- When reaching the throttled parent cfs_rq, the early throttled check skips update_load_avg() and h_nr_running updates
- This leaves the parent cfs_rq with stale load averages and incorrect h_nr_running counts
- The timing requires the group entity to stay enqueued despite throttling

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (driver uses CPU 0). Create nested cgroup hierarchy with CPU quota:
- In setup(): Use kstep_cgroup_create() for parent and child cgroups, set restrictive CPU quota on parent via kstep_cgroup_write()
- Create tasks with kstep_task_create() and assign to child cgroup using kstep_cgroup_add_task()
- In run(): Enqueue tasks to exhaust parent quota and trigger throttling
- Use kstep_tick_repeat() to advance time and let throttling take effect
- Monitor parent cfs_rq state via on_tick_begin() callback to verify group entity stays enqueued (on_rq=true)
- Enqueue/dequeue additional tasks in child cgroup to trigger the buggy path
- Log load_avg and h_nr_running values before/after operations to detect inconsistencies
- Success: Parent load_avg/h_nr_running remain stale; Fixed: Values update correctly despite throttling
