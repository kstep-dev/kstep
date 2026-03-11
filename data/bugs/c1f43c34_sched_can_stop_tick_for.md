# sched/fair: Fix sched_can_stop_tick() for fair tasks

- **Commit:** c1f43c342e1f2e32f0620bf2e972e2a9ea0a1e60
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** CFS (fair scheduling)

## Bug Description

The `sched_can_stop_tick()` function incorrectly determines when the scheduler tick can be stopped for fair (CFS) tasks. When multiple tasks are enqueued in the hierarchy (e.g., in cgroups), but only one task exists at the root level, the tick is incorrectly stopped. This prevents involuntary preemption between tasks that should be time-sliced, leading to potential scheduling delays and fairness violations.

## Root Cause

The code was checking `rq->cfs.nr_running > 1`, which only counts sched_entity objects at the root CFS level. However, it should check `rq->cfs.h_nr_running > 1`, which tracks all queued tasks across the entire hierarchy. Tasks can be organized in nested cgroups, so checking only the root level misses cases where multiple tasks exist deeper in the hierarchy.

## Fix Summary

Changed the condition from `rq->cfs.nr_running > 1` to `rq->cfs.h_nr_running > 1` to properly check the entire task hierarchy. This ensures the tick is not stopped when there are multiple fair tasks enqueued at any level in the hierarchy, guaranteeing that CFS tasks are properly time-sliced and preempted.

## Triggering Conditions

The bug occurs when there are multiple CFS tasks enqueued in nested cgroups but only one sched_entity at the root CFS runqueue level. This creates a state where `rq->cfs.nr_running = 1` (only one root-level entity) but `rq->cfs.h_nr_running > 1` (multiple tasks across the hierarchy). The scheduler incorrectly determines that the tick can be stopped since there's only "one task" at the root level, preventing involuntary preemption between tasks that should be time-sliced. This requires:
- Multiple CFS tasks organized in nested cgroups (child cgroups under root)
- Tasks distributed such that multiple child cgroup entities exist under a single parent
- NO_HZ_FULL configuration enabled to allow tick stopping
- All tasks must be runnable/enqueued simultaneously to trigger the condition

## Reproduce Strategy (kSTEP)

Use at least 2 CPUs (CPU 0 reserved). In `setup()`, create nested cgroups using `kstep_cgroup_create("parent")` and `kstep_cgroup_create("parent/child1")`, `kstep_cgroup_create("parent/child2")`. Create multiple CFS tasks with `kstep_task_create()` and assign them to different child cgroups using `kstep_cgroup_add_task()`. In `run()`, pin all tasks to CPU 1 with `kstep_task_pin()` and wake them with `kstep_task_wakeup()`. Use `kstep_tick_repeat()` to advance time and monitor the system state. In `on_tick_begin()` callback, check CPU 1's runqueue state by examining `cpu_rq(1)->cfs.nr_running` vs `cpu_rq(1)->cfs.h_nr_running` and log when `nr_running = 1` but `h_nr_running > 1`. Also monitor for lack of preemption by tracking if the same task remains current for too long despite multiple runnable tasks, indicating the tick was incorrectly stopped.
