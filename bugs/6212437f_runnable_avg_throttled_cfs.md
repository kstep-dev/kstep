# sched/fair: Fix runnable_avg for throttled cfs

- **Commit:** 6212437f0f6043e825e021e4afc5cd63e248a2b4
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When a cfs_rq is throttled or unthrottled but its group entity is not being dequeued/enqueued (i.e., when dequeue/enqueue is false), the runnable_avg and runnable_weight are not updated correctly. This leaves the scheduler hierarchy with stale averages that do not reflect the current h_nr_running, causing incorrect load balancing and scheduling decisions across the task group hierarchy.

## Root Cause

The code in `throttle_cfs_rq()` and `unthrottle_cfs_rq()` only called `update_load_avg()` and `se_update_runnable()` when actually dequeuing/enqueuing the group entity. When a throttled entity remains on the runqueue (dequeue=0 in throttle, enqueue=0 in unthrottle), these critical updates were skipped, leaving runnable_avg values out of sync with the actual h_nr_running that was being decremented/incremented at each hierarchy level.

## Fix Summary

The fix ensures that `update_load_avg()` and `se_update_runnable()` are always called to update the runnable average and runnable weight, even when the group entity is not being dequeued/enqueued. This is done by adding an else branch that calls these update functions before the h_nr_running counters are modified, ensuring the hierarchy maintains consistent load tracking during throttle/unthrottle operations.

## Triggering Conditions

This bug occurs during CFS bandwidth throttling in nested cgroup hierarchies. The key conditions are:
- A child cgroup with CPU bandwidth limits (quota/period) that becomes throttled
- Parent cgroup has sufficient load weight from other entities (prevents group entity dequeue)
- When `throttle_cfs_rq()` walks up the hierarchy, `dequeue` becomes 0 for parent levels
- The `update_load_avg()` and `se_update_runnable()` calls are skipped for non-dequeued entities
- This leaves runnable averages stale while h_nr_running is decremented, causing load tracking inconsistency
- Same issue occurs during unthrottle when `enqueue` is 0 but runnable averages aren't updated

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs. Create nested cgroup hierarchy with bandwidth throttling:
- Use `kstep_cgroup_create("parent")` and `kstep_cgroup_create("parent/child")` for hierarchy
- Set tight bandwidth limit on child: `kstep_cgroup_write("parent/child", "cpu.cfs_quota_us", "10000")`
- Create tasks: one for parent cgroup, multiple for child cgroup that will exhaust quota
- Pin parent task to CPU 1, child tasks to CPU 1 via `kstep_task_pin()` and `kstep_cgroup_add_task()`
- Use `kstep_tick_repeat()` to let child tasks consume their quota and trigger throttling
- Monitor with `on_tick_begin` callback: check `cfs_rq->throttled` status and compare `runnable_avg` vs `h_nr_running`
- Log discrepancies where runnable averages don't reflect actual hierarchy load after throttle
- Verify bug by checking that parent group entity's runnable stats become inconsistent when child throttles
