# Avoid multiple calling update_rq_clock() in __cfsb_csd_unthrottle()

- **Commit:** ebb83d84e49b54369b0db67136a5fe1087124dcc
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

After the introduction of async unthrottling for CFS bandwidth (commit 8ad075c2eb1f), the rq clock was being updated multiple times within loops that iterate over CFS runqueues—specifically in `__cfsb_csd_unthrottle()` and `unthrottle_offline_cfs_rqs()`. Each iteration triggers a call to `update_rq_clock()`, causing redundant clock updates within a single critical section and resulting in performance degradation and potential scheduler inconsistencies.

## Root Cause

The `unthrottle_cfs_rq()` function, called within the loop, internally invokes `update_rq_clock()`. When iterating over multiple CFS runqueues, this causes the clock to be updated repeatedly for each entry, even though a single clock update before the loop would suffice. The async unthrottling mechanism exposed this inefficiency by triggering multiple iterations over throttled CFS runqueues.

## Fix Summary

The fix ensures `update_rq_clock()` is called once before entering the loop, then sets the `RQCF_ACT_SKIP` flag (via `rq_clock_start_loop_update()`) to suppress further clock updates during iteration. After the loop completes, `rq_clock_stop_loop_update()` clears the flag, restoring normal clock update behavior. Two new helper functions are introduced to manage this flag cleanly.

## Triggering Conditions

The bug triggers when CFS bandwidth throttling is active and multiple CFS runqueues become throttled, then require async unthrottling via `__cfsb_csd_unthrottle()`. This occurs when:
- CFS bandwidth control is enabled with task groups that have CPU quota/period limits
- Tasks in these groups exceed their bandwidth allocation and get throttled
- Multiple throttled CFS runqueues accumulate on the throttled list
- Async unthrottling mechanism processes the list, calling `unthrottle_cfs_rq()` for each entry
- Each `unthrottle_cfs_rq()` call internally triggers `update_rq_clock()`, causing redundant updates
- The same issue occurs in `unthrottle_offline_cfs_rqs()` during CPU offlining scenarios

## Reproduce Strategy (kSTEP)

Use 3+ CPUs to create multiple throttled CFS runqueues. Setup creates task groups with tight bandwidth limits, triggers throttling by overloading, then forces async unthrottling:
- Create 3-4 task groups with `kstep_cgroup_create()` and set small quotas via `kstep_cgroup_write()`
- Pin tasks to different CPUs (1-3) using `kstep_task_pin()` and assign to groups with `kstep_cgroup_add_task()`
- Create CPU-intensive tasks that exceed bandwidth limits to trigger throttling
- Use `kstep_tick_repeat()` to advance time until multiple runqueues are throttled
- Monitor rq clock updates by hooking `on_tick_begin()` to log update_rq_clock() calls
- Trigger bandwidth replenishment to cause mass unthrottling and observe redundant clock updates
- Count clock updates during unthrottling loops - buggy kernel will show multiple updates per loop iteration
