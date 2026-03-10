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
