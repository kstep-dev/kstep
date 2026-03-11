# sched: Fix sched_delayed vs cfs_bandwidth

- **Commit:** 9b5ce1a37e904fac32d560668134965f4e937f6c
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When a bandwidth-throttled cgroup is unthrottled, the `unthrottle_cfs_rq()` function may encounter a scheduling entity that is in a "delayed dequeue" state (marked with `sched_delayed` flag). The original code did not properly handle this state, instead just warning about it via `SCHED_WARN_ON()` and attempting to break, which leads to "terminal scenarios" where the scheduler reaches inconsistent states. This manifests as kernel warnings/crashes during workloads that combine cgroup bandwidth limits with task wakeups.

## Root Cause

The bug occurs because when `sched_delayed` is set on an entity, it indicates the entity has been marked for dequeue but not yet fully dequeued. The original code checked `se->on_rq` first and warned if `sched_delayed` was also set, but this is an invalid assumption: an entity can legitimately have both flags set. When unthrottling occurs, the delayed dequeue must be finalized before attempting to wake the entity via `ENQUEUE_WAKEUP`, otherwise the scheduler attempts to enqueue an entity that is still partially in a dequeued state.

## Fix Summary

The fix reorders the logic to check for and handle `sched_delayed` state first. If an entity has a pending delayed dequeue, it is explicitly finished by calling `dequeue_entity()` with `DEQUEUE_SLEEP | DEQUEUE_DELAYED` flags before proceeding with the normal unthrottle path. This ensures the entity is fully dequeued and ready for the subsequent `ENQUEUE_WAKEUP`, preventing the scheduler from entering invalid states.

## Triggering Conditions

The bug occurs in the CFS bandwidth control subsystem during `unthrottle_cfs_rq()` when:
- A task group cgroup has bandwidth throttling enabled (cpu.cfs_quota_us and cpu.cfs_period_us)
- Tasks in the throttled cgroup are placed in delayed dequeue state (sched_delayed flag set)
- The cgroup bandwidth quota refill triggers unthrottling while entities have pending delayed dequeues
- The scheduler attempts to wake entities via ENQUEUE_WAKEUP before finishing the delayed dequeue
- This creates a race where entities are partially dequeued but scheduler tries to enqueue them
- Manifests as SCHED_WARN_ON() warnings and potential crashes during heavy bandwidth-limited workloads

## Reproduce Strategy (kSTEP)

Setup requires 2+ CPUs (CPU 0 reserved). Create bandwidth-throttled cgroup and tasks to trigger the race:
- Use `kstep_cgroup_create()` and `kstep_cgroup_write()` to set cpu.cfs_quota_us/period_us limits  
- Create multiple tasks with `kstep_task_create()` and add to cgroup via `kstep_cgroup_add_task()`
- Pin tasks to CPUs 1-N to consume bandwidth and trigger throttling
- Use `kstep_tick_repeat()` to advance time and exhaust the bandwidth quota
- Create timing where tasks enter delayed dequeue state just before bandwidth refill
- Monitor via `on_tick_begin()` callback to observe throttle/unthrottle transitions
- Log entity state (on_rq, sched_delayed flags) during unthrottling to detect the bug
- Success: SCHED_WARN_ON() triggered or inconsistent scheduler state during unthrottle
