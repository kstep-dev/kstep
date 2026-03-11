# sched/uclamp: Fix locking around cpu_util_update_eff()

- **Commit:** 93b73858701fd01de26a4a874eb95f9b7156fd4b
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (uclamp/cgroup)

## Bug Description

The `cpu_cgroup_css_online()` function calls `cpu_util_update_eff()` without holding the `uclamp_mutex` or `rcu_read_lock()` that are required by the function. This creates a race condition where concurrent operations could modify the cgroup hierarchy while `cpu_util_update_eff()` is traversing it, potentially causing data corruption or incorrect propagation of uclamp values to new cgroups.

## Root Cause

`cpu_util_update_eff()` requires two critical synchronization primitives: the `uclamp_mutex` to protect against concurrent reads and writes to the cgroup hierarchy, and `rcu_read_lock()` to safely traverse cgroup data structures. While other call sites correctly acquire these locks before calling the function, `cpu_cgroup_css_online()` was missing both, creating an unprotected access window.

## Fix Summary

The fix adds mutex and RCU locking around the `cpu_util_update_eff()` call in `cpu_cgroup_css_online()`. Additionally, assertions (`lockdep_assert_held()` and `SCHED_WARN_ON()`) are added within `cpu_util_update_eff()` to document and enforce the lock requirements, catching future violations at runtime.

## Triggering Conditions

This bug occurs in the uclamp (utilization clamping) subsystem when new cgroups are brought online. The race condition requires:
- CONFIG_UCLAMP_TASK_GROUP enabled in kernel config
- Concurrent cgroup creation via `cpu_cgroup_css_online()` and uclamp operations modifying the cgroup hierarchy
- The timing window where `cpu_util_update_eff()` traverses cgroup data structures without proper synchronization
- Multiple CPUs or preemption points allowing concurrent access to the cgroup hierarchy
- Tasks with uclamp constraints to make the uclamp subsystem active and increase contention on shared data structures

The race manifests when `cpu_util_update_eff()` reads cgroup data concurrently with writers that hold `uclamp_mutex`, potentially causing inconsistent reads or accessing freed/modified cgroup structures during RCU-protected traversal.

## Reproduce Strategy (kSTEP)

Create concurrent cgroup operations to maximize the race window:
- Use at least 3 CPUs (CPU 0 reserved, CPUs 1-2 for tasks)
- In `setup()`: Create multiple tasks with different uclamp constraints using `kstep_task_create()` and configure them with varying utilization clamp values
- In `run()`: Create a parent cgroup with `kstep_cgroup_create("parent")`, then add tasks with `kstep_cgroup_add_task()`
- Use `kstep_tick_repeat()` to advance time and allow cgroup operations to settle
- Rapidly create child cgroups with `kstep_cgroup_create("child1")`, `kstep_cgroup_create("child2")` while moving tasks between them
- Monitor with `on_tick_begin()` callback to log cgroup hierarchy state and detect inconsistencies
- Check for kernel warnings, RCU stalls, or assertion failures that indicate the unlocked access to cgroup data structures
- Repeat the cgroup create/destroy cycle multiple times to increase chances of hitting the race condition timing window
