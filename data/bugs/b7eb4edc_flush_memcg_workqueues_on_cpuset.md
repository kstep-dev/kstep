# sched/isolation: Flush memcg workqueues on cpuset isolated partition change

- **Commit:** b7eb4edcc3b5cd7ffdcbd56fa7a12de41b39424d
- **Affected file(s):** kernel/sched/isolation.c, kernel/sched/sched.h
- **Subsystem:** core (isolation/housekeeping)

## Bug Description

When the HK_TYPE_DOMAIN housekeeping cpumask is modified at runtime to isolate CPUs, memcg workqueues may still have pending or executing work on the newly isolated CPU. This creates a synchronization issue where asynchronous memcg draining operations could interfere with the isolation guarantees, potentially causing workqueue tasks to execute on CPUs that should be isolated from kernel activity.

## Root Cause

The `housekeeping_update()` function updates the HK_TYPE_DOMAIN cpumask with RCU synchronization, but fails to flush memcg workqueues after the change. Since memcg workqueues are queued to the main per-CPU workqueue pool, any pending work on the affected CPUs must be explicitly drained to prevent races between the isolation change and remaining enqueued work.

## Fix Summary

The fix adds a call to `mem_cgroup_flush_workqueue()` in `housekeeping_update()` after the RCU synchronization point, ensuring memcg workqueues are fully flushed before the isolation configuration takes effect. This synchronizes the memcg subsystem with the new isolation boundaries and prevents async operations from executing on isolated CPUs.

## Triggering Conditions

The bug occurs when the HK_TYPE_DOMAIN housekeeping cpumask is modified at runtime to isolate CPUs while memcg workqueues have pending or executing work on the target CPUs. Specific conditions include:
- Multi-CPU system where CPU isolation can be dynamically configured via cpuset partitions
- Active memory cgroup operations that queue asynchronous work to per-CPU workqueue pools
- Runtime modification of the housekeeping cpumask (e.g., through cpuset isolated partition changes)
- Race condition between `housekeeping_update()` RCU synchronization and memcg workqueue execution
- The newly isolated CPU still has pending memcg drain work that violates isolation guarantees

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved for driver, CPUs 1-2 for isolation testing):
- In `setup()`: Create memory cgroups using `kstep_cgroup_create()` and allocate tasks with `kstep_task_create()`
- Add tasks to different cgroups with `kstep_cgroup_add_task()` to trigger memcg activity
- In `run()`: Generate memcg workqueue activity by moving tasks between cgroups and triggering memory pressure
- Simulate runtime housekeeping mask changes (requires kernel-level hooks or direct manipulation)
- Use `on_tick_begin()` callback to monitor workqueue execution on target CPUs before/after isolation
- Detection: Check if memcg-related work items execute on CPUs that should be isolated
- Log workqueue activity and CPU isolation state changes to identify synchronization violations
