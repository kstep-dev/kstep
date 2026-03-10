# sched/topology: Fix sched_domain_topology_level alloc in sched_init_numa()

- **Commit:** 71e5f6644fb2f3304fcb310145ded234a37e7cc1
- **Affected file(s):** kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

A previous commit introduced an off-by-one error in the memory allocation for `sched_domain_topology_level` arrays in `sched_init_numa()`. The allocation was missing a `+1` to account for a sentinel/terminating element, causing insufficient memory to be allocated. This led to an Oops (kernel crash) on Arm64 systems when `__sdt_free()` attempted to iterate through the topology levels and access memory beyond the allocated buffer.

## Root Cause

The code allocates space for `i + nr_levels` topology level structures, where `i` is the count of default topology entries and `nr_levels` is the count of NUMA-distance topology levels. However, the array is used with an implicit sentinel element (similar to a null-terminated string), requiring space for `i + nr_levels + 1` elements. Without the additional slot, the loop in `__sdt_free()` that iterates through `for_each_sd_topology(tl)` attempts to read past the allocated memory region.

## Fix Summary

The fix adds `+ 1` to the allocation size, changing from `(i + nr_levels)` to `(i + nr_levels + 1)`. This ensures sufficient memory is allocated to accommodate the sentinel element at the end of the topology level array, preventing the out-of-bounds memory access during domain cleanup.

## Triggering Conditions

This bug is triggered during NUMA domain initialization on systems with multiple NUMA nodes. The conditions are:
- System with NUMA topology (multiple nodes with different distances)  
- `sched_init_numa()` called during scheduler initialization
- Multiple NUMA distance levels (`nr_levels > 0`) detected
- Default topology entries exist (`i > 0` from `sched_domain_topology`)
- Subsequent domain cleanup via `__sdt_free()` → `for_each_sd_topology(tl)` loop
- The loop iterates past allocated memory due to missing sentinel element
- Out-of-bounds access when dereferencing `*per_cpu_ptr(sdd->sd, j)`

The race occurs between allocation in `sched_init_numa()` and cleanup in `__sdt_free()` when domain construction fails or during normal teardown.

## Reproduce Strategy (kSTEP)

To reproduce this bug, create a multi-node NUMA topology and trigger domain rebuilds:

- Use 4+ CPUs (CPU 0 reserved for driver, need 3+ for multi-node setup)
- In `setup()`: Use `kstep_topo_init()` then `kstep_topo_set_node()` with multiple node configurations like `{"0", "1", "2-3", "4"}` to create 3 NUMA nodes
- Call `kstep_topo_apply()` to force topology reconstruction and trigger `sched_init_numa()`
- Use callback `on_sched_balance_begin()` to monitor domain operations  
- Create tasks with `kstep_task_create()` and pin to different nodes with `kstep_task_pin()`
- Trigger additional domain rebuilds by changing CPU topology or capacities
- Monitor for kernel oops/crash during domain cleanup phases
- Log topology state with `kstep_topo_print()` to verify multi-level NUMA setup
- Detection: System crash in `__sdt_free()` or memory corruption symptoms
