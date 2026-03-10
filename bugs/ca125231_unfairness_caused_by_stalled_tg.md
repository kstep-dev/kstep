# Fix unfairness caused by stalled tg_load_avg_contrib when the last task migrates out

- **Commit:** ca125231dd29fc0678dd3622e9cdea80a51dffe4
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When a task migrates out of a cgroup, the total group load average (`tg->load_avg`) can become stale and abnormal, causing scheduling unfairness. The issue occurs because `__update_blocked_fair()` may prematurely remove cfs_rqs from the leaf_cfs_rq_list even though their load average contributions have not yet been propagated to the parent task group, violating the invariant that `tg->load_avg := \Sum tg->cfs_rq[]->avg.load_avg`.

## Root Cause

Due to the 1ms update rate limiting in `update_tg_load_avg()`, there is a window where a cfs_rq's load_avg has decreased but this reduction hasn't been reflected in `tg->load_avg`. The function `cfs_rq_is_decayed()` checks whether a cfs_rq has fully decayed (and can be safely removed from tracking), but it fails to check whether `cfs_rq->tg_load_avg_contrib` is non-zero, meaning there is still a pending load contribution waiting to be propagated. Consequently, `__update_blocked_fair()` removes such cfs_rqs from the leaf list before the update has occurred.

## Fix Summary

The fix adds a check in `cfs_rq_is_decayed()` to verify that `cfs_rq->tg_load_avg_contrib` is zero before marking a cfs_rq as fully decayed. This ensures cfs_rqs with pending load contributions remain in the leaf_cfs_rq_list until their contributions have been properly updated to the parent task group, maintaining load average consistency.

## Triggering Conditions

- A task must migrate out of a cgroup, causing the cfs_rq's load_avg to decay
- The migration timing must occur within the 1ms rate-limiting window of `update_tg_load_avg()`
- The cfs_rq's `tg_load_avg_contrib` must be non-zero when `cfs_rq_is_decayed()` is called
- `__update_blocked_fair()` must run during this window and attempt to remove the cfs_rq from leaf_cfs_rq_list
- The cfs_rq's avg.load_avg should be fully decayed (zero) but `tg_load_avg_contrib` remains stale
- Multiple cgroups with task migration patterns create higher probability of hitting the race condition
- The bug is observable as inconsistent `tg->load_avg` values that don't match sum of constituent cfs_rq load averages

## Reproduce Strategy (kSTEP)

1. **Setup**: Use at least 3 CPUs (CPU 0 reserved). Create two cgroups using `kstep_cgroup_create()` 
2. **Initial state**: Create multiple tasks with `kstep_task_create()`, assign to first cgroup with `kstep_cgroup_add_task()`
3. **Load accumulation**: Use `kstep_tick_repeat(100)` to let tasks accumulate load_avg in the cgroup's cfs_rq
4. **Migration trigger**: Rapidly migrate all tasks out using `kstep_cgroup_add_task()` to move to second cgroup
5. **Race window**: Immediately call `kstep_tick()` multiple times to trigger `__update_blocked_fair()` during 1ms update window
6. **Detection**: Use custom callback in `on_sched_softirq_end()` to check `cfs_rq->tg_load_avg_contrib` vs `cfs_rq->avg.load_avg`
7. **Verification**: Compare sum of all cfs_rq->avg.load_avg against `tg->load_avg` to detect inconsistency
8. **Timing**: Use `step_interval_us = 100` for fine-grained control of the race condition timing
