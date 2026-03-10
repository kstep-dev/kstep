# sched/fair: Fix warning in bandwidth distribution

- **Commit:** 2f8c62296b6f656bbfd17e9f1fadd7478003a9d9
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

A race condition in `distribute_cfs_runtime()` causes a `SCHED_WARN_ON(cfs_rq->runtime_remaining > 0)` warning to be triggered. When CPU 0 distributes bandwidth and makes a local cfs_rq's runtime positive but defers its unthrottling, CPU 1 can observe the same cfs_rq as throttled with positive runtime and no CSD pending state, violating the expected invariant and triggering the warning.

## Root Cause

The original code uses a simple pointer (`local_unthrottle`) to track the local cfs_rq needing async unthrottle, and defers its unthrottling until after the RCU-protected traversal of the throttled list. A concurrent CPU executing `distribute_cfs_runtime()` can inspect the throttled_list while the local cfs_rq is in an intermediate state: throttled, with positive runtime_remaining, but not yet queued on the CSD list for async unthrottle. This violates the invariant checked by `SCHED_WARN_ON(cfs_rq->runtime_remaining > 0)`.

## Fix Summary

The fix changes the local unthrottle tracking from a single pointer to a list, allowing the local cfs_rq to be added to a temporary list during the RCU-protected traversal. This ensures other CPUs attempting concurrent bandwidth distribution will see the cfs_rq is scheduled for unthrottle (by being on the local list), preventing them from reobserving the inconsistent state and triggering the warning.

## Triggering Conditions

This race occurs in the CFS bandwidth control subsystem during concurrent `distribute_cfs_runtime()` execution on multiple CPUs. The triggering conditions require:
- At least two CPUs running bandwidth distribution concurrently (via slack timer or period timer)
- CFS bandwidth control enabled with cgroups having `cpu.cfs_quota_us` set
- Tasks that consume their bandwidth quota and get throttled
- Precise timing where CPU A is in the middle of distributing runtime to its local cfs_rq (making `runtime_remaining > 0`) but hasn't yet queued it for async unthrottling
- CPU B simultaneously inspects the throttled list and finds CPU A's cfs_rq in the inconsistent state: throttled, positive runtime, not on CSD list
- The race window exists between setting `runtime_remaining > 0` and adding the cfs_rq to the unthrottle list

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs total (CPU 0 reserved for driver, CPUs 1-2 for the race):
1. In `setup()`: Create cgroup with bandwidth limits using `kstep_cgroup_create("test")` and `kstep_cgroup_write("test", "cpu.cfs_quota_us", "50000")` (50% quota)
2. Create multiple CPU-bound tasks with `kstep_task_create()` and pin them across CPUs 1-2 using `kstep_task_pin()`
3. Add tasks to the bandwidth-limited cgroup with `kstep_cgroup_add_task("test", task->pid)`
4. In `run()`: Start tasks with `kstep_task_wakeup()` and run them until throttled via `kstep_tick_repeat(100)`
5. Use `on_tick_begin` callback to monitor cfs_rq states and detect when `runtime_remaining > 0` while throttled
6. Trigger concurrent bandwidth distribution by manipulating the slack/period timers to fire simultaneously
7. Log the warning condition: cfs_rq throttled with positive runtime but not on CSD list
8. Success indicator: `SCHED_WARN_ON(cfs_rq->runtime_remaining > 0)` triggered in kernel logs
