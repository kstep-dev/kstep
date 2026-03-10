# sched: Fix get_push_task() vs migrate_disable()

- **Commit:** e681dcbaa4b284454fecd09617f8b24231448446
- **Affected file(s):** kernel/sched/sched.h
- **Subsystem:** Core scheduler (RT/migration)

## Bug Description

The `get_push_task()` function is used by `push_rt_task()` to find a task that can be migrated away when a higher-priority task with migration disabled is pending on the current CPU. However, the function only checked if `nr_cpus_allowed == 1` but did not check whether the task itself had migration disabled. This caused pointless invocations of the migration thread, which would correctly observe that the task could not be moved and fail to perform any action.

## Root Cause

The `get_push_task()` function was missing a check for the `migration_disabled` flag on the task being evaluated. Even though the function was intended to return only tasks that could be migrated, it did not validate whether the candidate task had migration disabled. This allowed tasks with migration disabled to pass through the filter, leading to wasted migration thread invocations that would ultimately fail.

## Fix Summary

The fix adds a simple check: if the task has `migration_disabled` set, `get_push_task()` now returns NULL instead of the task. This prevents unnecessary migration thread invocations when the current task cannot be moved, improving efficiency by eliminating pointless work.

## Triggering Conditions

- **Scheduler subsystem**: RT task balancing, specifically the `push_rt_task()` code path
- **Task configuration**: Current running task must be RT class with `migration_disabled=1` but `nr_cpus_allowed > 1`
- **Triggering event**: A higher priority RT task becomes runnable and has `migration_disabled=1`, causing the scheduler to attempt pushing the current task away via `get_push_task()`
- **Race condition**: The bug manifests when `get_push_task()` incorrectly returns a migration-disabled task, leading to futile migration thread invocation
- **Required state**: At least 2 CPUs needed, with RT tasks on different CPUs to enable push/pull balancing

## Reproduce Strategy (kSTEP)

- **CPUs needed**: Minimum 3 (CPU 0 reserved, use CPUs 1-2 for RT tasks)
- **Setup**: Create two RT FIFO tasks with `kstep_task_fifo()` and pin them to different CPUs initially
- **Sequence**: 
  1. Pin task A to CPU 1, start it running with `kstep_task_wakeup()`
  2. Use migration disable mechanism on task A (may need manual state manipulation or cgroup constraints)
  3. Create higher-priority RT task B with migration disabled, wakeup on same CPU as A
  4. Monitor `get_push_task()` calls via scheduler callbacks or manual state inspection
- **Detection**: Log when `get_push_task()` returns a task despite `migration_disabled=1`, and verify migration thread gets invoked but fails to move the task
- **Observation**: Use `on_sched_softirq_begin/end` callbacks to track migration attempts and their outcomes
