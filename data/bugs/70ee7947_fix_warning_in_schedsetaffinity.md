# sched: fix warning in sched_setaffinity

- **Commit:** 70ee7947a29029736a1a06c73a48ff37674a851b
- **Affected file(s):** kernel/sched/syscalls.c
- **Subsystem:** core

## Bug Description

A spurious WARN_ON_ONCE() is triggered in normal operation when a race condition occurs between a cpuset update and a per-task affinity assignment. Specifically, when a cpuset cgroup changes the allowed CPUs and simultaneously a task's affinity is being set, the requested affinity can become incompatible with the cpuset mask. The kernel correctly handles this by falling back to the cpuset mask, but the warning fires even though this is a legitimate race condition with a proper fallback mechanism in place.

## Root Cause

A previous commit (8f9ea86fdf99b) added logic to warn when the cpuset mask has no overlap with the requested task affinity. However, this race condition is trivial to trigger and is not indicative of a bug—the code already has a fallback that handles it correctly. The WARN_ON_ONCE() was incorrectly flagging a normal, expected scenario as an error condition.

## Fix Summary

The fix removes the WARN_ON_ONCE() check and replaces it with a simple conditional. The corrected mask is still applied in the no-overlap case, but no warning is generated since this is expected behavior, not a bug.

## Triggering Conditions

The bug occurs in the `__sched_setaffinity()` syscall path when a race condition happens between cpuset cgroup updates and per-task CPU affinity assignments. Specifically:

- A task must be assigned to a cpuset cgroup that constrains allowed CPUs
- Concurrent operations: one thread repeatedly modifies the cpuset's allowed CPUs, another thread repeatedly calls `sched_setaffinity()` on the task
- The race timing must result in the requested task affinity having zero overlap with the current cpuset mask
- The SCA_USER flag must be set and a previous `user_mask` must exist, causing `cpumask_and(new_mask, new_mask, ctx->user_mask)` to return an empty mask
- This triggers the spurious `WARN_ON_ONCE(empty)` in the fallback path that handles cpuset/affinity conflicts

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved for driver, use CPUs 1-3 for test):

1. **Setup**: Create a cpuset cgroup with `kstep_cgroup_create("test_cpuset")` and set initial CPUs with `kstep_cgroup_set_cpuset("test_cpuset", "1-2")`
2. **Task creation**: Use `kstep_task_create()` to create a target task and add it to the cpuset with `kstep_cgroup_add_task("test_cpuset", task_pid)`
3. **Race setup**: Create two competing kthreads - one that repeatedly toggles cpuset CPUs between "1-2" and "1" using `kstep_cgroup_set_cpuset()`, another that repeatedly sets task affinity to CPU 2 only
4. **Execution**: Use `kstep_tick_repeat()` to let the race run for sufficient iterations (hundreds of cycles)
5. **Detection**: Monitor for kernel warning messages in logs. The bug manifests as `WARN_ON_ONCE()` output when the empty mask condition is hit during the race
6. **Validation**: On fixed kernels, the same race should occur without generating warnings, demonstrating the spurious nature of the original warning
