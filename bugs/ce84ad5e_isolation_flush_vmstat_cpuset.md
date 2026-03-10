# sched/isolation: Flush vmstat workqueues on cpuset isolated partition change

- **Commit:** ce84ad5e994aea5d41ff47135a71439ad4f54005
- **Affected file(s):** kernel/sched/isolation.c, kernel/sched/sched.h
- **Subsystem:** core (isolation/housekeeping)

## Bug Description

When the HK_TYPE_DOMAIN housekeeping cpumask is modified at runtime to change CPU isolation status, asynchronous vmstat workqueue operations may still be pending or executing on a CPU that has just been made isolated. This violates the isolation guarantee that such CPUs should have no background work running on them, potentially causing unexpected scheduler or memory management behavior on isolated CPUs.

## Root Cause

The `housekeeping_update()` function was synchronizing against the RCU subsystem and flushing the memory cgroup workqueue after changing the isolation mask, but it was missing the corresponding synchronization with the vmstat workqueue. Since vmstat work is scheduled asynchronously on CPUs, a race condition exists where the isolation mask change is committed but vmstat operations are still in flight on the newly isolated CPU.

## Fix Summary

The fix adds a call to `vmstat_flush_workqueue()` in the `housekeeping_update()` function after the RCU synchronization and memory cgroup flush, ensuring all pending vmstat workqueue operations complete before the isolation mask change becomes visible to the system.

## Triggering Conditions

The bug occurs when the HK_TYPE_DOMAIN housekeeping cpumask is modified at runtime via cpuset isolated partition changes. Key conditions include:
- Runtime modification of CPU isolation mask through housekeeping_update() 
- Pending or executing vmstat workqueue operations on CPUs being newly isolated
- Race condition where isolation mask change commits before vmstat work completion
- The missing synchronization with mm_percpu_wq workqueue (used for vmstat operations)
- Timing window between mem_cgroup_flush_workqueue() and final isolation enforcement
- Any memory management activity that schedules vmstat work before the isolation change

## Reproduce Strategy (kSTEP)

Reproducing this race requires simulating runtime cpuset isolation changes with concurrent vmstat activity:
- Use 3+ CPUs (CPU 0 reserved, CPU 1-2+ for isolation testing)  
- In setup(): Create memory pressure tasks using kstep_task_create() to trigger vmstat workqueue activity
- Use kstep_cgroup_create() to set up cpuset cgroups for isolation testing
- In run(): Start tasks with memory allocation patterns using kstep_task_wakeup()
- Simulate isolation changes by manipulating cpuset.cpus through kstep_cgroup_write()
- Use kstep_tick_repeat() with short intervals to create timing windows
- Detect bug via on_tick_begin() callback checking for unexpected vmstat work on isolated CPUs
- Log vmstat work queue state before/after isolation changes
- Monitor for isolation violations: background vmstat operations on supposedly isolated CPUs
