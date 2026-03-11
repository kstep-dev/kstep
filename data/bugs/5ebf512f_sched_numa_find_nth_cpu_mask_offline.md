# sched: Fix sched_numa_find_nth_cpu() if mask offline

- **Commit:** 5ebf512f335053a42482ebff91e46c6dc156bf8c
- **Affected file(s):** kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

The `sched_numa_find_nth_cpu()` function crashes with a kernel panic when all CPUs in a given mask are offline. This occurs because the function uses `bsearch()` to find the closest CPU in NUMA masks, but does not check if `bsearch()` returns NULL when no matching entry is found. When all CPUs are offline, the search fails and returns NULL, which is then dereferenced without validation, causing a kernel page fault and panic.

## Root Cause

The `bsearch()` function returns NULL when it cannot find an entry matching the search criteria. In this case, if all CPUs in the provided cpus mask are offline, there is no intersection between `sched_domains_numa_masks` and the offline cpus mask. The original code immediately uses the return value (`hop_masks - k.masks`) without checking for NULL, resulting in a null pointer dereference and kernel panic.

## Fix Summary

The fix adds a null pointer check immediately after the `bsearch()` call. If `hop_masks` is NULL, the function jumps to the unlock label and returns `nr_cpu_ids` (the default initialized value), allowing the caller to handle the "no available CPU" condition gracefully via existing error handling logic in `smp_call_function_any()`.

## Triggering Conditions

The bug requires `sched_numa_find_nth_cpu()` to be called with a CPU mask where all CPUs are offline, causing no intersection between `sched_domains_numa_masks` and the provided mask. This typically occurs during early boot when:
- `smp_call_function_any()` searches for a CPU to execute a callback (e.g., PMU initialization)
- NUMA topology is configured with multiple domains 
- Boot parameters like `maxcpus=N` limit online CPUs, leaving some CPUs offline
- The caller's mask includes only offline CPUs, leading to NULL from `bsearch()`
- The subsequent pointer arithmetic `hop_masks - k.masks` dereferences NULL

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved). In `setup()`, create a NUMA topology with `kstep_topo_set_node()` to establish multiple NUMA domains with offline CPUs. In `run()`, simulate the problematic call path by directly invoking `sched_numa_find_nth_cpu()` via exported symbol with a CPU mask containing only offline CPUs (use `kstep_write()` to offline CPUs via sysfs). Use `on_tick_begin()` callback to monitor for kernel panic or NULL pointer dereference in logs. Check return value - the buggy version will crash before returning, while fixed version returns `nr_cpu_ids`. Log the function entry/exit to detect the crash, and verify the fix by ensuring graceful error handling without kernel panic.
