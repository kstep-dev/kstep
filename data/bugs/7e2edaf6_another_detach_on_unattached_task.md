# Fix another detach on unattached task corner case

- **Commit:** 7e2edaf61814fb6aa363989d718950c023b882d4
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

A task that has been selected for migration by `try_to_wake_up()` but is awaiting actual wake-up by `sched_ttwu_pending()` can be detached from its CFS runqueue while its load tracking has not been attached. This occurs when `task_change_group_fair()` is invoked (e.g., due to cgroup changes) during the window between migration and final wake-up, causing the scheduler to attempt a detach operation on a task whose load tracking state is uninitialized.

## Root Cause

When a task is selected for migration in `try_to_wake_up()`, the function calls `migrate_task_rq_fair()` which unattaches the task's load tracking via `remove_entity_load_avg()` and resets `se->avg.last_update_time = 0`, indicating the task is not yet attached. However, if `task_change_group_fair()` is subsequently called before the task is actually woken up and attached, the `detach_entity_cfs_rq()` function unconditionally calls `detach_entity_load_avg()` on an unattached task, leading to load tracking corruption.

## Fix Summary

The fix adds a check in `detach_entity_cfs_rq()` to verify that `se->avg.last_update_time != 0` before proceeding with the detach operation. If the load tracking has not been attached yet, the function returns early, preventing the problematic detach operation on unattached tasks.

## Triggering Conditions

This bug occurs in the CFS scheduler during a specific race window involving task migration and cgroup changes:

- A sleeping task must be woken up by `try_to_wake_up()` on a different CPU, triggering migration via `select_task_rq()`
- During migration, `migrate_task_rq_fair()` unattaches the task's load tracking (`se->avg.last_update_time = 0`)
- The task is queued for wake-up via `ttwu_queue_wakelist()` but not yet processed by `sched_ttwu_pending()`
- Before the actual wake-up occurs, `task_change_group_fair()` is called (triggered by cgroup movement)
- This causes `detach_entity_cfs_rq()` to attempt detaching load tracking on an already unattached task
- The bug manifests as load tracking corruption when `detach_entity_load_avg()` operates on uninitialized state

Critical requirements: SMP system, task migration across CPUs, concurrent cgroup operations during the migration window.

## Reproduce Strategy (kSTEP)

The reproduction requires at least 2 CPUs (CPU 0 reserved for driver) and coordinated timing between task migration and cgroup changes:

**Setup (2+ CPUs):** Create a task pinned to CPU 1, then set up CPU topology and create multiple cgroups using `kstep_cgroup_create()`.

**Triggering sequence in run():**
1. `kstep_task_pause(task)` - put task to sleep on CPU 1
2. `kstep_task_pin(task, 2, 2)` - trigger migration: try_to_wake_up() will select CPU 2, call migrate_task_rq_fair() 
3. **Critical timing:** Before `kstep_tick()` processes the wake-up queue, call `kstep_cgroup_add_task()` to move the task to a different cgroup
4. `kstep_tick()` - this should trigger task_change_group_fair() before sched_ttwu_pending() processes the wake-up

**Detection:** Use `on_tick_begin()` callback to log task's `se->avg.last_update_time` and check for load tracking corruption. Monitor for warnings or crashes in detach_entity_load_avg(). The bug occurs when detach operations happen on tasks with `last_update_time == 0`.
