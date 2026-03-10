# sched/debug: fix dentry leak in update_sched_domain_debugfs

- **Commit:** c2e406596571659451f4b95e37ddfd5a8ef1d0dc
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** debug

## Bug Description

The `update_sched_domain_debugfs()` function uses the pattern `debugfs_remove(debugfs_lookup())` to remove debugfs entries during scheduler domain updates. This pattern leaks a dentry reference, causing memory exhaustion during hotplug stress tests as the leaked dentries accumulate over time.

## Root Cause

The `debugfs_lookup()` function returns a reference to a dentry that must be properly released. The `debugfs_remove()` function does not handle the reference counting from `debugfs_lookup()` correctly when used in this pattern, causing the reference to remain unreleased. This leads to dentry objects being retained in memory indefinitely.

## Fix Summary

Replace the `debugfs_remove(debugfs_lookup(buf, sd_dentry))` pattern with the dedicated `debugfs_lookup_and_remove(buf, sd_dentry)` function, which properly handles dentry reference counting and ensures the reference is released after removal.

## Triggering Conditions

The bug is triggered in the `update_sched_domain_debugfs()` function during scheduler domain updates, primarily during CPU hotplug operations (CPU online/offline events). Each call to this function removes existing debugfs entries using the faulty `debugfs_remove(debugfs_lookup())` pattern, leaking one dentry reference per CPU processed. The leak occurs in the loop `for_each_cpu(cpu, sd_sysctl_cpus)` where debugfs directories for each CPU are removed and recreated. Under stress testing with repeated hotplug operations, these leaked dentry references accumulate rapidly, eventually exhausting system memory. The debugfs subsystem must be enabled and the scheduler debug infrastructure must be initialized for the bug to manifest.

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce directly with kSTEP since it involves debugfs memory leaks rather than core scheduler behavior. A potential approach would require extending kSTEP to monitor memory usage and trigger scheduler domain updates:

1. Use at least 2 CPUs (CPU 1+ available for hotplug simulation)
2. In `setup()`: Initialize topology with `kstep_topo_init()` and `kstep_topo_apply()`
3. In `run()`: Repeatedly trigger scheduler domain updates by changing CPU topology configurations using `kstep_topo_set_*()` functions followed by `kstep_topo_apply()`, simulating the domain reconstruction that occurs during hotplug
4. Use `on_tick_begin()` callback to periodically check memory allocation statistics or dentry counts (would require kernel instrumentation)
5. Detection: Monitor for increasing memory usage patterns or add custom kernel logging to track dentry reference counts in the debugfs removal path
6. Alternative: Instrument the kernel to count `debugfs_lookup()` calls vs `dput()` calls to detect the reference leak directly

Note: Full reproduction may require kernel modifications to expose memory leak detection, as the bug primarily affects kernel memory management rather than observable scheduler behavior.
