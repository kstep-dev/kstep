# sched/topology: fix KASAN warning in hop_cmp()

- **Commit:** 01bb11ad828b320749764fa93ad078db20d08a9e
- **Affected file(s):** kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

The `hop_cmp()` function unconditionally initializes `prev_hop` by dereferencing `(struct cpumask ***)b - 1`, even when `b` points to the first element of the `masks` array. When `b == k->masks`, this pointer arithmetic and dereference attempts to access memory before the array, causing KASAN to detect an out-of-bounds memory access.

## Root Cause

The `prev_hop` variable was initialized unconditionally at the start of the function with `*((struct cpumask ***)b - 1)`, but the condition that determines whether this value should actually be used (`b == k->masks`) was checked later in the code. This causes an invalid memory dereference when `b` points to the first element, as the code attempts to access the non-existent element at `b - 1`.

## Fix Summary

The fix moves the `prev_hop` initialization into a conditional block that checks `b == k->masks` first. When `b` equals `k->masks` (the first element), the function sets `k->w = 0` and returns early, avoiding the invalid dereference. Only when `b != k->masks` does the code safely initialize and use `prev_hop`.

## Triggering Conditions

The bug occurs in the NUMA topology subsystem during CPU selection operations. The `hop_cmp()` function is used as a comparison function in `sched_numa_find_nth_cpu()` for binary search through NUMA distance levels. The triggering conditions are:

- NUMA topology must be configured with multiple nodes and distance levels
- The system must call `sched_numa_find_nth_cpu()` which internally uses `hop_cmp()` for searching
- The comparison function gets invoked with `b == k->masks` (pointing to the first element of the masks array)
- At this point, the unconditional dereference of `(struct cpumask ***)b - 1` accesses memory before the array
- KASAN detects this out-of-bounds memory access during the pointer arithmetic and dereference
- The race condition occurs when NUMA-aware scheduling decisions trigger CPU search operations across different NUMA distance hops

## Reproduce Strategy (kSTEP)

To reproduce this KASAN warning using kSTEP:

**Setup Requirements:**
- At least 4 CPUs (CPU 0 reserved for driver, CPUs 1-4 for NUMA nodes)
- Configure NUMA topology with multiple nodes to trigger `sched_numa_find_nth_cpu()` calls

**kSTEP Implementation:**
1. In `setup()`: Use `kstep_topo_init()` and `kstep_topo_set_node()` to create NUMA topology with nodes ["0", "1-2", "1-2", "3-4", "3-4"]
2. Call `kstep_topo_apply()` to activate the NUMA configuration
3. Create tasks with `kstep_task_create()` and distribute them across different NUMA nodes using `kstep_task_pin()`
4. In `run()`: Trigger NUMA-aware operations by migrating tasks between nodes or creating CPU affinity constraints
5. Use `kstep_tick_repeat()` to advance scheduler state and trigger NUMA distance calculations
6. **Detection**: Monitor kernel logs for KASAN warnings about out-of-bounds memory access in `hop_cmp()`
7. **Observation**: The bug manifests as a KASAN report when the comparison function is called with the first array element during CPU search operations
