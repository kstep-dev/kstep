# sched_ext: Fix out-of-bounds access in scx_idle_init_masks()

- **Commit:** 2a064262eb378263792cf1fb044de631ac41bcc5
- **Affected file(s):** kernel/sched/ext_idle.c
- **Subsystem:** EXT

## Bug Description

The `scx_idle_node_masks` array is allocated with `num_possible_nodes()` elements but is indexed by NUMA node IDs via `for_each_node()`. On systems with non-contiguous NUMA node numbering (e.g., nodes 0 and 4), the node IDs can exceed the allocated array size, resulting in out-of-bounds memory access and corruption. This affects systems where NUMA nodes are not numbered sequentially from zero.

## Root Cause

The allocation function uses `num_possible_nodes()`, which returns the count of NUMA nodes, but the array is indexed with actual NUMA node IDs via `for_each_node()`. When NUMA nodes are non-contiguous (holes exist in the numbering), node IDs can be larger than the count, causing array indexing to exceed bounds. The correct size for an array indexed by node ID is `nr_node_ids`, which represents the maximum node ID range.

## Fix Summary

Replace `num_possible_nodes()` with `nr_node_ids` when allocating the `scx_idle_node_masks` array. This ensures the array is large enough to accommodate all possible node IDs, preventing out-of-bounds access on systems with non-contiguous NUMA node numbering.

## Triggering Conditions

The bug is triggered during scheduler extension initialization in `scx_idle_init_masks()` on systems with non-contiguous NUMA node numbering. The code path executes when sched_ext is initialized and allocates per-node idle CPU masks. The trigger requires:
- NUMA topology with gaps in node numbering (e.g., nodes 0 and 4 exist, but 1-3 do not)
- Scheduler extension (sched_ext) initialization, which calls `scx_idle_init_masks()`
- The `for_each_node(i)` loop iterates over actual node IDs (0, 4), not sequential indices
- Array access `scx_idle_node_masks[i]` uses node ID as index, causing out-of-bounds access when node ID 4 exceeds array size allocated for 2 nodes
- Memory corruption occurs when writing to `scx_idle_node_masks[4]` beyond the allocated array bounds

## Reproduce Strategy (kSTEP)

This bug occurs during kernel initialization and cannot be easily reproduced with kSTEP's runtime manipulation APIs, as it requires specific NUMA topology setup that triggers the sched_ext initialization path. A potential approach would be:
- Use `kstep_topo_init()` and `kstep_topo_set_node()` to create a sparse NUMA topology with non-contiguous node IDs
- Trigger scheduler extension initialization if available in the kernel version
- Monitor memory access patterns during initialization using kernel debugging facilities
- However, this specific bug is better reproduced through boot-time NUMA configuration rather than runtime kSTEP simulation
- Alternative: Create a test driver that directly calls the vulnerable `scx_idle_init_masks()` function with a controlled NUMA topology setup to observe the out-of-bounds array access
- Use memory debugging tools (KASAN) to detect the corruption when the array bounds are exceeded
