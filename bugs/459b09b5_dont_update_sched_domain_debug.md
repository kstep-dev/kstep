# sched/debug: Don't update sched_domain debug directories before sched_debug_init()

- **Commit:** 459b09b5a3254008b63382bf41a9b36d0b590f57
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** debug

## Bug Description

When scheduler topology is rebuilt due to CPU capacity asymmetry detected via ACPI (which happens during device initcall), the `update_sched_domain_debugfs()` function may be invoked before `sched_debug_init()` has created the debugfs sched root directory. This causes `debugfs_create_dir("domains", NULL)` to create the directory at the debugfs root instead of under /sys/kernel/debug/sched/, resulting in the debugfs hierarchy being incorrect (/sys/kernel/debug/domains instead of /sys/kernel/debug/sched/domains).

## Root Cause

The function `update_sched_domain_debugfs()` can be called from topology initialization code (via `arch_topology.c::init_cpu_capacity_callback()`) before `sched_debug_init()` runs. Since `sched_debug_init()` is a late initcall and ACPI frequency detection is a device initcall (which runs earlier), the parent debugfs directory (`debugfs_sched`) may not yet exist. When a NULL parent is passed to `debugfs_create_dir()`, the directory is created at the debugfs root level rather than as a child of the sched directory.

## Fix Summary

The fix adds an early return guard in `update_sched_domain_debugfs()` that checks if `debugfs_sched` (the sched root directory) has been created yet. If it hasn't, the function returns without attempting to create or update the domains debugfs directory, preventing the incorrect directory creation. The check is placed before any debugfs operations that depend on the root being initialized.

## Triggering Conditions

This bug occurs during kernel initialization when the scheduler topology is rebuilt due to CPU capacity asymmetry detection. Specifically:
- The bug manifests when `update_sched_domain_debugfs()` is called from `arch_topology.c::init_cpu_capacity_callback()` during a device initcall
- This happens before `sched_debug_init()` (a late initcall) has executed to create the `/sys/kernel/debug/sched/` directory  
- The `debugfs_sched` global pointer is still NULL at the time of the topology rebuild
- ACPI-based frequency asymmetry detection triggers this early topology rebuild
- Without the fix, `debugfs_create_dir("domains", NULL)` creates `/sys/kernel/debug/domains` instead of `/sys/kernel/debug/sched/domains`

## Reproduce Strategy (kSTEP)

This bug is difficult to reproduce with kSTEP since it occurs during early kernel initialization before kSTEP drivers run. However, a verification approach could:
- Set up a minimal system with 2 CPUs using `kstep_topo_init()` and `kstep_topo_apply()`
- Use `kstep_cpu_set_capacity()` to create asymmetric CPU capacities that would trigger topology rebuilds
- Monitor debugfs directory creation by checking for presence of scheduler debug files in the wrong location
- Create tasks with `kstep_task_create()` and run `kstep_tick_repeat(10)` to ensure scheduler domains are active
- Use custom callbacks to detect when topology changes occur and verify correct debugfs hierarchy
- Check if `/sys/kernel/debug/sched/domains` exists vs `/sys/kernel/debug/domains` via filesystem inspection
- The detection would need custom kernel logging to verify the debugfs creation path taken
