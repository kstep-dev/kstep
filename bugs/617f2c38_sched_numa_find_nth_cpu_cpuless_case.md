# sched/topology: Fix sched_numa_find_nth_cpu() in CPU-less case

- **Commit:** 617f2c38cb5ce60226042081c09e2ee3a90d03f8
- **Affected file(s):** kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

When `sched_numa_find_nth_cpu()` is called with a CPU-less NUMA node, the kernel attempts to dereference an uninitialized entry in `sched_domains_numa_masks`, causing a kernel crash. The bug occurs because CPU-less nodes do not have their corresponding records initialized in the topology masks, but the function directly uses the provided node without validation.

## Root Cause

The function did not account for CPU-less NUMA nodes when accessing `sched_domains_numa_masks[hop][node]`. When a node has no CPUs, its entry in the masks array is never populated during initialization, so dereferencing it results in accessing uninitialized memory and causes a crash.

## Fix Summary

The fix calls `numa_nearest_node(node, N_CPU)` to find the nearest NUMA node that actually contains CPUs, then uses that node for all subsequent mask operations. This ensures the function only dereferences valid, initialized entries in `sched_domains_numa_masks`.

## Triggering Conditions

The bug triggers when `sched_numa_find_nth_cpu()` is called with a CPU-less NUMA node. The NUMA topology must contain at least one node that has no CPUs assigned. When the function attempts to access `sched_domains_numa_masks[hop][node]` for a CPU-less node, it dereferences an uninitialized entry causing a kernel crash. This can occur during scheduler operations that use NUMA-aware CPU selection, load balancing across NUMA domains, or when userspace calls that indirectly trigger NUMA CPU lookup with invalid node parameters. The timing issue is deterministic once the invalid node parameter is passed to the function.

## Reproduce Strategy (kSTEP)

Create a NUMA topology with CPU-less nodes using at least 3 CPUs (CPU 0 reserved for driver). In setup(), call `kstep_topo_init()` then `kstep_topo_set_node()` to configure nodes like ["0", "1-2", "", "3"] where node 2 is CPU-less. Apply with `kstep_topo_apply()`. In run(), create tasks with `kstep_task_create()` and pin them to different NUMA nodes using `kstep_task_pin()`. Trigger scheduler operations that might call `sched_numa_find_nth_cpu()` through load balancing by creating CPU-intensive tasks and calling `kstep_tick_repeat()` to advance time. Use `on_sched_balance_begin()` callback to log balance operations across NUMA domains. Monitor for kernel crashes or unexpected behavior when the scheduler attempts to access the CPU-less node during NUMA-aware operations. The bug manifests as immediate kernel panic when the uninitialized mask entry is dereferenced.
