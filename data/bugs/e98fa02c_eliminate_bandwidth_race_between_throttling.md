# sched/fair: Eliminate bandwidth race between throttling and distribution

- **Commit:** e98fa02c4f2ea4991dae422ac7e34d102d2f0599
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

A race condition exists in the CFS bandwidth throttling mechanism where a runqueue begins throttling before quota is added to the bandwidth pool, but completes throttling after distribute_cfs_runtime() finishes. The throttled entity becomes invisible to the distribution function because it wasn't on the throttled list when distribution executed, resulting in rare period-length stalls where the entity doesn't receive available quota.

## Root Cause

The throttle_cfs_rq() function did not check if bandwidth became available during the throttling process. Additionally, there was a timing window where an entity could start throttling but not yet be added to the throttled list when distribute_cfs_runtime() runs, causing it to be missed entirely during quota distribution. This race occurs because the throttled list addition and the distribution mechanism were not properly synchronized.

## Fix Summary

The fix refactors runtime assignment into __assign_cfs_rq_runtime() to allow checking bandwidth availability while holding the cfs_b->lock. throttle_cfs_rq() now checks if bandwidth has become available before completing the throttle—if bandwidth is available, it aborts throttling entirely (returning false); otherwise, it adds the entity to the throttled list while still holding the lock to ensure visibility to subsequent distributions. This eliminates the race by guaranteeing that any entity needing throttling is either not throttled at all (because bandwidth appeared) or is atomically added to the throttled list where it cannot be missed.

## Triggering Conditions

The race occurs when a CFS runqueue exhausts its runtime and begins throttling while distribute_cfs_runtime() executes concurrently. The sequence requires: (1) A task group with limited CFS bandwidth quota depleting its runtime, triggering throttle_cfs_rq(); (2) The throttling process starting but not yet adding the runqueue to cfs_b->throttled_cfs_rq list; (3) distribute_cfs_runtime() running simultaneously, scanning the throttled list but missing the in-progress throttling runqueue; (4) The distribution finishing before throttling completes, leaving the runqueue throttled despite available bandwidth. This timing window is narrow but results in period-length stalls when the runqueue misses quota distribution.

## Reproduce Strategy (kSTEP)

Create a task group with tight bandwidth limits using kstep_cgroup_create() and kstep_cgroup_set_weight(). Setup multiple CPUs (at least 3: driver on CPU 0, tasks on CPUs 1-2) to enable concurrent throttling and distribution. In setup(), create bandwidth-limited cgroups and spawn CPU-intensive tasks using kstep_task_create(), then pin tasks to different CPUs with kstep_task_pin(). In run(), use kstep_tick_repeat() to consume bandwidth rapidly until near exhaustion, then trigger concurrent throttle/distribute timing by carefully controlling task wakeups with kstep_task_wakeup(). Use on_tick_begin() callback to monitor cfs_rq->throttled state and cfs_b->throttled_cfs_rq list membership. Detection involves checking if any runqueue becomes throttled (cfs_rq->throttled=1) without appearing on the throttled list, indicating the race condition occurred.
