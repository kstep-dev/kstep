# sched/fair: Prevent unlimited runtime on throttled group

- **Commit:** 2a4b03ffc69f2dedc6388e9a6438b5f4c133a40d
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** CFS (fair scheduling)

## Bug Description

When a running task is moved to a throttled task group with no other runnable tasks on the CPU, the task can continue running at 100% CPU despite the cgroup having allocated bandwidth limits. The throttled cfs_rq should prevent the task from running, but instead the task remains as the current entity without proper scheduling constraints applied.

## Root Cause

During sched_move_task(), the sequence dequeues the task, calls sched_change_group() to move it to a new group, and then only enqueues the task if the group is not throttled. When the destination group is throttled, the task is not enqueued and group entities are not queued up the hierarchy. However, set_next_task() still marks the task as current without triggering a reschedule to evaluate whether the task should actually be allowed to run under the new throttle constraints.

## Fix Summary

The fix adds a resched_curr() call after set_next_task() in the sched_move_task() function. This immediately triggers a reschedule check, allowing the scheduler to evaluate the throttling constraints and prevent the task from running unlimited when it should be throttled.

## Triggering Conditions

The bug occurs when a currently running task is moved via sched_move_task() to a throttled cgroup while being the only runnable task on that CPU. The key conditions are:
- A task must be actively running (task_current() returns true) on a CPU
- The destination cgroup must be throttled (bandwidth quota exhausted, cfs_rq throttled)  
- No other runnable tasks exist on the same CPU to trigger natural rescheduling
- The sched_move_task() path: task gets dequeued, moved to throttled group, but set_next_task() marks it current without reschedule
- Group entities are not enqueued in the throttled cfs_rq, leaving root runnable_load_avg at zero
- Without external reschedule events, the task continues running at 100% CPU indefinitely

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved). Create a bandwidth-limited cgroup and move a running task into it when throttled:
- In setup(): Create task with kstep_task_create(), pin to CPU 1 with kstep_task_pin(task, 1, 1)  
- Create cgroup with kstep_cgroup_create("test_group"), set bandwidth limit with kstep_cgroup_write("test_group", "cpu.cfs_quota_us", "10000")
- In run(): Start task with kstep_task_wakeup(task), let it consume CPU time via kstep_tick_repeat()
- Throttle the cgroup by exceeding quota, then use kstep_cgroup_add_task("test_group", task->pid) to trigger sched_move_task()
- Use on_tick_begin() callback to monitor CPU usage via kstep_output_nr_running()
- Check if task continues running at 100% CPU despite being in throttled cgroup (bug manifestation)
- Compare CPU utilization before/after move - should drop to 0 with fix, stays at 100% without fix
