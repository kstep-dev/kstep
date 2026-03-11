# sched_ext: Avoid NULL scx_root deref through SCX_HAS_OP()

- **Commit:** a77d10d032f511b027d80ef0640309a73e2561fa
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

When `scx_root` is dynamically allocated (as planned in future changes), it can become NULL, yet certain code paths attempt to dereference it through `SCX_HAS_OP(scx_root, ...)` without proper NULL checks. This leads to a NULL pointer dereference crash in functions like `handle_hotplug()` and `scx_tg_offline()` during CPU hotplug or cgroup operations.

## Root Cause

The `SCX_HAS_OP()` macro dereferences `scx_root->has_op` to test the bitmap. While `scx_root` is currently statically allocated and always valid, transitioning to dynamic allocation means certain code paths (specifically `handle_hotplug()` and `scx_tg_offline()`) can execute when `scx_root` is NULL without verifying its validity first.

## Fix Summary

Added explicit NULL checks before dereferencing `scx_root` in vulnerable code paths. In `handle_hotplug()`, a direct NULL check is added at the beginning. In `scx_tg_offline()`, a `scx_cgroup_enabled` check precedes the `SCX_HAS_OP()` call to guarantee `scx_root` validity.

## Triggering Conditions

This bug occurs when the sched_ext scheduler is in the process of being dynamically allocated/deallocated while CPU hotplug events or cgroup operations happen concurrently. The specific conditions are:

- CPU hotplug operations (online/offline) executing when `scx_root` is NULL during scheduler initialization/teardown
- Cgroup task group operations (`scx_tg_offline()`) accessing `SCX_HAS_OP()` when sched_ext is not fully enabled
- Race between scheduler loading/unloading and these operations, where `scx_root` transitions to NULL
- The timing window between scheduler teardown setting `scx_root = NULL` and completion of cleanup operations
- Code paths that bypass `scx_enabled()` checks but still attempt to use `SCX_HAS_OP()` macro

## Reproduce Strategy (kSTEP)

This bug requires simulating the race between sched_ext scheduler lifecycle and CPU/cgroup operations:

- **CPUs needed:** At least 2 CPUs (CPU 1+ for hotplug simulation, CPU 0 reserved for driver)
- **Setup:** Create a minimal topology with `kstep_topo_init()` and `kstep_topo_apply()`
- **Approach:** Since kSTEP operates with a static kernel, directly simulate the NULL `scx_root` condition by:
  1. Use `kstep_task_create()` to create tasks that would trigger cgroup operations
  2. Manually set up conditions that would call `handle_hotplug()` path via CPU state changes
  3. Create cgroups with `kstep_cgroup_create()` and manipulate task assignments with `kstep_cgroup_add_task()`
  4. Monitor for NULL pointer dereference crashes or kernel panics in callback functions
- **Detection:** Use `on_tick_begin()` and `on_sched_softirq_begin()` callbacks to log system state
- **Validation:** Look for kernel panic messages or system instability when accessing `SCX_HAS_OP()` without proper NULL checks
