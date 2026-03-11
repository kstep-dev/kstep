# sched/uclamp: Fix a bug in propagating uclamp value in new cgroups

- **Commit:** 7226017ad37a888915628e59a84a2d1e57b40707
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** uclamp

## Bug Description

When a new cgroup is created, the effective uclamp value is not updated to reflect the hierarchy constraints. Instead, it defaults to the root task group's values (1024 for both uclamp_min and uclamp_max), causing the runqueue to be clamped to maximum frequency. This results in the system running at maximum frequency unnecessarily, wasting power even when performance requirements do not justify it.

## Root Cause

The cpu_cgroup_css_online() function was missing a call to cpu_util_update_eff() to propagate the hierarchical uclamp constraints when a new cgroup transitions to online state. Without this call, newly created cgroups fail to inherit the effective uclamp values from their parent cgroup hierarchy, leaving them at the permissive default values.

## Fix Summary

The fix adds a call to cpu_util_update_eff(css) in cpu_cgroup_css_online() to ensure that when a new cgroup becomes online, it properly inherits and applies the effective uclamp values from its parent cgroup hierarchy. This forces recalculation of the effective constraints based on the cgroup hierarchy rather than using the permissive defaults.

## Triggering Conditions

This bug occurs during cgroup hierarchy management when `CONFIG_UCLAMP_TASK_GROUP` is enabled:
- A parent cgroup exists with restricted uclamp values (uclamp_min/max < 1024)
- A new child cgroup is created under this parent hierarchy  
- The cpu cgroup subsystem transitions the new cgroup to online state via `cpu_cgroup_css_online()`
- Without the fix, `cpu_util_update_eff()` is never called for the new cgroup
- The child cgroup's effective uclamp values remain at default root_task_group values (1024, 1024)
- Tasks in the new cgroup bypass the parent's uclamp constraints, causing unnecessary maximum frequency scaling
- The race condition manifests immediately upon cgroup creation - no specific task scheduling or timing dependencies required

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Strategy:
1. **Setup**: Create parent cgroup with restrictive uclamp_max (e.g., 512), verify hierarchical constraint propagation
2. **Create child cgroup**: Use `kstep_cgroup_create("child")` to trigger `cpu_cgroup_css_online()`
3. **Check effective values**: Access cgroup's task_group effective uclamp values via internal scheduler structures  
4. **Add task**: Use `kstep_cgroup_add_task("child", task_pid)` to move a task into the buggy cgroup
5. **Trigger frequency scaling**: Use `kstep_task_wakeup()` and `kstep_tick_repeat()` to activate the task and observe CPU frequency behavior
6. **Detection**: Check if runqueue's uclamp_max matches child cgroup's incorrect value (1024) instead of parent's constraint (512)
7. **Validation**: Compare CPU frequency scaling behavior between parent and child cgroups - child should incorrectly scale to maximum
8. **Callbacks**: Use `on_tick_begin()` to log cgroup effective uclamp values and frequency scaling decisions
