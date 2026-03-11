# sched_ext: Add scx_cgroup_enabled to gate cgroup operations and fix scx_tg_online()

- **Commit:** 568894edbe48f0878f787ed533dc9dbfd09c0fbe
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

If a BPF scheduler implements `ops.cgroup_exit()` but does not implement `ops.cgroup_init()`, the `scx_tg_online()` function fails to set the `SCX_TG_INITED` flag. This causes `ops.cgroup_exit()` to never be called from `scx_tg_offline()`, resulting in incomplete cleanup of cgroup state and potential resource leaks.

## Root Cause

The code used `SCX_HAS_OP(cgroup_init)` for two conflicting purposes: (1) to test whether SCX cgroup operations are enabled globally, and (2) to test whether the specific `cgroup_init()` operation exists. When a scheduler implements `cgroup_exit()` but not `cgroup_init()`, the `SCX_TG_INITED` flag is never set, causing `cgroup_exit()` to be skipped despite being implemented.

## Fix Summary

Introduce a separate `scx_cgroup_enabled` flag to gate whether cgroup operations are enabled, independent of whether `cgroup_init()` is implemented. Update `scx_tg_online()` to set `SCX_TG_INITED` whenever cgroup operations are enabled (if no error occurs), ensuring `cgroup_exit()` is properly called. Consistently use `scx_cgroup_enabled` throughout cgroup-related functions for clarity and correctness.

## Triggering Conditions

This bug occurs when a BPF sched_ext scheduler implements `ops.cgroup_exit()` but not `ops.cgroup_init()`. The problematic sequence is:
- sched_ext cgroup support is enabled (`scx_cgroup_enabled = true`)
- A cgroup (task_group) comes online, triggering `scx_tg_online()`
- `SCX_HAS_OP(cgroup_init)` returns false, so `scx_tg_online()` skips calling `cgroup_init()` and fails to set `SCX_TG_INITED` flag
- When the cgroup goes offline, `scx_tg_offline()` checks both `SCX_HAS_OP(cgroup_exit)` AND `SCX_TG_INITED` before calling `cgroup_exit()`
- Since `SCX_TG_INITED` was never set, `cgroup_exit()` is never called despite being implemented

## Reproduce Strategy (kSTEP)

This requires kernel compilation with CONFIG_EXT_GROUP_SCHED enabled and a custom BPF scheduler. The kSTEP driver should:
- Use 2+ CPUs (CPU 0 reserved for driver)
- Create multiple cgroups with `kstep_cgroup_create()` to trigger `scx_tg_online()`
- Monitor `task_group->scx_flags` field to verify `SCX_TG_INITED` flag state
- Use `on_sched_group_alloc()` callback to trace cgroup creation and flag setting
- Create tasks with `kstep_task_create()` and assign to cgroups with `kstep_cgroup_add_task()`
- Destroy cgroups to trigger `scx_tg_offline()` path
- Log whether `cgroup_exit()` is called by instrumenting the BPF scheduler or using kernel tracepoints
- Verify the bug by confirming `SCX_TG_INITED` is never set despite cgroup operations being enabled
