# sched/numa: Avoid migrating task to CPU-less node

- **Commit:** 5c7b1aaf139dab5072311853bacc40fc3457d1f9
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NUMA balancing (scheduler)

## Bug Description

In a memory tiering system (e.g., PMEM + DRAM), some NUMA nodes may contain only memory without CPUs. The NUMA balancing policy incorrectly selects CPU-less nodes as migration targets when they accumulate the most page faults, causing tasks to be placed on nodes where they cannot actually execute. This results in unreasonable task placement that defeats the purpose of NUMA optimization.

## Root Cause

The code iterates through all online NUMA nodes using `for_each_online_node()` and selects the node with the highest fault count as the preferred NUMA node, without checking whether the selected node has CPUs. On systems with memory tiering, this allows CPU-less nodes to be chosen as migration targets, even though tasks cannot run there.

## Fix Summary

Replace `for_each_online_node()` with `for_each_node_state(nid, N_CPU)` to only iterate through nodes with CPUs. Additionally, add explicit logic in `task_numa_placement()` to detect when the selected node is CPU-less and redirect to the nearest CPU-containing node instead.

## Triggering Conditions

The bug occurs in `task_numa_migrate()` during NUMA balancing when:
- System has memory tiering topology with CPU-less NUMA nodes (e.g., PMEM + DRAM)
- NUMA balancing is enabled (`/proc/sys/kernel/numa_balancing = 1`)
- Task generates significant page faults on memory from CPU-less nodes
- `task_numa_migrate()` searches for alternative migration targets when preferred node lacks space
- The loop `for_each_online_node(nid)` considers CPU-less nodes as valid targets
- Task gets incorrectly placed on node where it cannot execute

## Reproduce Strategy (kSTEP)

Need 3+ CPUs with mixed CPU/memory-only NUMA topology:
1. **Setup**: Use `kstep_topo_init()` and `kstep_topo_set_node()` to create nodes [1-2] with CPUs and [3] memory-only
2. **Tasks**: Create tasks with `kstep_task_create()` and pin to CPU nodes initially
3. **Memory pressure**: Use `kstep_cgroup_create()` to allocate memory on node 3, trigger page faults
4. **NUMA balancing**: Enable with `kstep_sysctl_write("kernel/numa_balancing", "1")`
5. **Migration trigger**: Call `kstep_tick_repeat()` to advance NUMA scanning periods
6. **Detection**: Monitor task placement with `on_tick_begin()` callback and `kstep_output_curr_task()`
7. **Bug verification**: Check if tasks end up with `numa_preferred_nid = 3` (CPU-less node)
Use `TRACE_INFO()` to log when `task_numa_migrate()` considers CPU-less nodes as targets
