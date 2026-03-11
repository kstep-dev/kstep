# sched/numa: Fix NUMA topology for systems with CPU-less nodes

- **Commit:** 0fb3978b0aac3a5c08637aed03cc2d65f793508f
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c, kernel/sched/sched.h, kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

The scheduler incorrectly identifies NUMA topology type for systems containing CPU-less NUMA nodes (such as persistent memory nodes). On systems with both CPU-containing and CPU-less nodes, the topology classification algorithm includes distances from CPU-less nodes, causing it to misclassify the topology as NUMA_GLUELESS_MESH when it should be NUMA_DIRECT or NUMA_BACKPLANE. This leads to incorrect NUMA parameters (sched_numa_topology_type, sched_domains_numa_levels, and sched_max_numa_distance) that affect task placement and load balancing decisions.

## Root Cause

The `init_numa_topology_type()` function uses `for_each_online_node()` to iterate over all online nodes when determining the NUMA topology type. This includes CPU-less nodes, which should not participate in topology classification since they cannot host runnable tasks. When comparing distances between nodes to detect intermediate nodes or topology patterns, including CPU-less nodes distorts the distance relationships and causes incorrect classification.

## Fix Summary

The fix modifies `init_numa_topology_type()` and `sched_init_numa()` to use a new `for_each_cpu_node_but()` macro that iterates only over nodes with CPUs (N_CPU state), excluding the given offline_node. Additionally, the code now re-initializes NUMA topology parameters during CPU hotplug events via the new `sched_update_numa()` function called from `sched_cpu_activate()` and `sched_cpu_deactivate()`, ensuring topology remains correct as nodes transition between having and not having CPUs.

## Triggering Conditions

The bug triggers when: (1) The system has CPU-less NUMA nodes (persistent memory nodes) that are online, (2) `init_numa_topology_type()` is called during CPU hotplug or NUMA reconfiguration, (3) The distance matrix between nodes creates false intermediary relationships due to CPU-less nodes being included in topology calculations, (4) The algorithm incorrectly classifies the topology as NUMA_GLUELESS_MESH instead of NUMA_DIRECT/NUMA_BACKPLANE based on distorted distance comparisons. The specific trigger occurs in the nested loops of `init_numa_topology_type()` when `for_each_online_node()` includes CPU-less nodes in distance analysis, causing the scheduler to set incorrect sched_numa_topology_type, sched_domains_numa_levels, and sched_max_numa_distance values.

## Reproduce Strategy (kSTEP)

Requires 4+ CPUs (CPU 0 reserved for driver). Use `kstep_topo_init()` and `kstep_topo_set_node()` to create 4 NUMA nodes with distance matrix: node 0 (CPUs 1-2), node 1 (CPUs 3-4), node 2 (CPU-less), node 3 (CPU-less), distances matching the example: [10,21,17,28], [21,10,28,17], [17,28,10,28], [28,17,28,10]. In `setup()`, create tasks with `kstep_task_create()` and pin them to CPU nodes. In `run()`, trigger NUMA reconfiguration by simulating CPU hotplug or directly calling the topology initialization path. Monitor `sched_numa_topology_type` via kernel debugging or by observing scheduler behavior changes. Detect the bug by checking if topology type is incorrectly set to NUMA_GLUELESS_MESH (value 2) when it should be NUMA_DIRECT (value 0) for this simple 2-socket topology.
