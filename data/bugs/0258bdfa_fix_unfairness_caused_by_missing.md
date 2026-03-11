# Fix unfairness caused by missing load decay

- **Commit:** 0258bdfaff5bd13c4d2383150b7097aecd6b6d82
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

When an idle task is attached to a cgroup and then moved to another CPU before being woken up, the task's load is transferred to cfs_rq->removed but is not properly propagated and decayed from the parent task_group. This causes the parent cgroup to appear to have constant load, making vruntime calculations for sibling cfs_rq's tasks increase faster than they should. In real-world container scenarios, this leads to severe CPU time unfairness—equally weighted cgroups can receive vastly different allocations (e.g., 99% vs 1% CPU time instead of 50/50).

## Root Cause

The `propagate_entity_cfs_rq()` function was not properly ensuring that load updates and cfs_rq leaf list updates were applied when tasks were moved between CPUs. The original code would break early on throttled cfs_rqs without propagating updates up the hierarchy, and it didn't add the cfs_rq back to the leaf list when it was previously removed. This meant orphaned load from moved tasks could persist in the hierarchy indefinitely, skewing fairness calculations.

## Fix Summary

The fix ensures proper load propagation by: (1) adding the current cfs_rq to the leaf list before starting the propagation loop, (2) updating load averages and adding parent cfs_rqs to the leaf list for non-throttled queues (with continue to process further ancestors), and (3) for throttled cfs_rqs, only adding to the leaf list and breaking if it was newly added. This guarantees that load changes are fully propagated up the cgroup hierarchy and the accounting remains consistent.

## Triggering Conditions

The bug occurs in the CFS load propagation path when an idle task undergoes cross-CPU migration due to cpuset enforcement before being woken up. Specifically: (1) an idle task is attached to a cgroup via `attach_entity_cfs_rq()`, accumulating load on the initial cfs_rq, (2) a cpuset constraint forces the task to migrate to another CPU before wakeup, transferring load to `cfs_rq->removed` but leaving stale load in the parent task_group hierarchy, (3) `propagate_entity_cfs_rq()` fails to properly decay this orphaned load due to early breaks on throttled cfs_rqs and missing leaf list management. The result is phantom load that skews vruntime calculations for sibling cfs_rqs, causing severe fairness violations (99%/1% CPU splits instead of 50%/50%).

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved for driver). Create two sibling cgroups with equal weights (100) and restricted cpusets: cg-1 pinned to CPU 1, cg-2 pinned to CPU 2. In setup(), use `kstep_cgroup_create("cg-1")`, `kstep_cgroup_set_weight("cg-1", 100)`, `kstep_cgroup_set_cpuset("cg-1", "1")` and similarly for cg-2. In run(), create an idle task with `kstep_task_create()`, attach it to cg-1 with `kstep_cgroup_add_task("cg-1", task->pid)`, then immediately change its cpuset to CPU 2 with `kstep_cgroup_set_cpuset("cg-1", "2")` before calling `kstep_task_wakeup()`. Create active tasks in both cgroups and measure their CPU time distribution over several hundred ticks. Use `on_tick_begin` to log per-cgroup load and vruntime progression. The bug manifests as severely skewed CPU allocation (e.g., 90%/10%) instead of fair 50%/50% sharing between equally weighted cgroups.
