# psi: Fix psi state corruption when schedule() races with cgroup move

- **Commit:** d583d360a620e6229422b3455d0be082b8255f5e
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

A race condition in PSI state tracking corrupts internal PSI counters when `cgroup_move_task()` runs concurrently with `schedule()`. This manifests as kernel warnings (task underflow, inconsistent task state) and bogusly high IO pressure readings. The bug occurs because `cgroup_move_task()` inspects the task's scheduler state while PSI updates from deactivation are deferred and not yet applied.

## Root Cause

The offending commit (4117cebf1a9f) batched PSI callbacks in `schedule()` to reduce cgroup tree updates. However, between deactivating a task and completing the task switch, `pick_next_task()` may drop the runqueue lock for load balancing. During this window, `cgroup_move_task()` can run after the task is physically dequeued but before PSI updates are applied. Since `cgroup_move_task()` relies on scheduler state (task_on_rq_queued, task->in_iowait) that no longer reflects the actual PSI state, it fails to migrate all relevant PSI flags, leading to counter underflows and leaks in both old and new cgroups.

## Fix Summary

The fix changes `cgroup_move_task()` to use the cached `task->psi_flags` instead of inferring PSI state from scheduler state variables. This eliminates the race condition by relying on the task's coherent PSI state rather than potentially mismatched scheduler state, preventing counter corruptions and stale pressure readings.

## Triggering Conditions

The race requires precise timing between `schedule()` and `cgroup_move_task()` in the PSI subsystem. The vulnerable window occurs when:
- A task is being deactivated in `schedule()` after `deactivate_task()` clears `p->on_rq = 0`
- PSI updates (TSK_RUNNING, TSK_IOWAIT) are deferred until `psi_sched_switch()` 
- `pick_next_task()` drops the runqueue lock for load balancing
- During this unlocked window, `cgroup_move_task()` executes and inspects scheduler state
- The task appears dequeued (`!task_on_rq_queued`) but PSI flags haven't been updated yet
- `cgroup_move_task()` infers incorrect PSI state from scheduler variables vs cached `task->psi_flags`
- This causes PSI counter leaks in the old cgroup and underflows in the new cgroup
- The bug manifests as kernel warnings about task underflows and inconsistent PSI state

## Reproduce Strategy (kSTEP)

Reproduce this race by creating conditions where `schedule()` drops the rq lock during task switching while simultaneously moving tasks between cgroups:
- Setup: At least 2 CPUs (CPU 0 reserved for driver), enable PSI, create 2 cgroups  
- Use `kstep_cgroup_create("test1")` and `kstep_cgroup_create("test2")`
- Create tasks with `kstep_task_create()` and add to first cgroup via `kstep_cgroup_add_task()`
- Configure topology to trigger load balancing with `kstep_topo_init()` and asymmetric CPU setup
- In `run()`: Start tasks that will trigger scheduling and load balancing via `kstep_task_wakeup()`
- Use `kstep_tick_repeat()` to advance scheduler state and create load imbalance
- During active scheduling, move tasks between cgroups using `kstep_cgroup_add_task()` 
- Use `on_tick_begin()` callback to log PSI state and detect counter inconsistencies
- Check for kernel warnings in logs indicating PSI underflows or "inconsistent task state"
- Verify bug reproduction by observing PSI counter corruption between old/new cgroups
