# sched_ext: idle: Fix scx_bpf_pick_any_cpu_node() behavior

- **Commit:** fde7d64766c1142fe79507bbef4968cee274cc0b
- **Affected file(s):** kernel/sched/ext_idle.c
- **Subsystem:** EXT

## Bug Description

When the `SCX_PICK_IDLE_IN_NODE` flag is specified, `scx_bpf_pick_any_cpu_node()` should return a CPU from the specified node regardless of idle state. However, the original code would fall back to `cpumask_any_distribute(cpus_allowed)` when no idle CPU is found, potentially returning a CPU from a different node than requested. This violates the API contract and breaks scheduler extensions that rely on node-local CPU selection.

## Root Cause

After `scx_pick_idle_cpu()` fails to find an idle CPU, the fallback path unconditionally uses `cpumask_any_distribute(cpus_allowed)`, which picks from the entire allowed cpumask across all nodes. The code does not check the `SCX_PICK_IDLE_IN_NODE` flag to constrain the fallback search to the target node only, causing node-affinity violations.

## Fix Summary

The fix adds a conditional check: when `SCX_PICK_IDLE_IN_NODE` is set, the fallback uses `cpumask_any_and_distribute(cpumask_of_node(node), cpus_allowed)` to ensure the selected CPU is from the target node. For non-node-constrained calls, the original behavior is preserved.

## Triggering Conditions

The bug triggers when `scx_bpf_pick_any_cpu_node()` is called with the `SCX_PICK_IDLE_IN_NODE` flag set, on a multi-node NUMA system where:
- The target node has no idle CPUs in the allowed cpumask
- `scx_pick_idle_cpu()` fails to find an idle CPU in the target node  
- The fallback path executes, using `cpumask_any_distribute(cpus_allowed)` instead of constraining to the target node
- The allowed cpumask spans multiple NUMA nodes
- A scheduler extension relies on strict node-local CPU selection for correctness

This occurs during sched_ext BPF program execution when selecting CPUs for task placement or load balancing operations that require node affinity guarantees.

## Reproduce Strategy (kSTEP)

**Note:** This bug is specific to the sched_ext subsystem, which kSTEP doesn't directly support. However, we can create a conceptual reproduction strategy:

**Setup:** Multi-node NUMA topology with at least 4 CPUs across 2 nodes using `kstep_topo_set_node()`. Create tasks on all CPUs of node 0 to make them non-idle.

**Reproduction approach:** Since kSTEP targets CFS rather than sched_ext, direct reproduction isn't possible. Instead, we could:
1. Use `kstep_topo_init()` and `kstep_topo_set_node()` to create a 2-node system  
2. Pin tasks to node 0 CPUs to eliminate idle CPUs: `kstep_task_pin(task, 1, 2)`
3. Monitor CPU selection decisions during load balancing via `on_sched_balance_selected()`
4. Log when CPUs are selected from unexpected nodes during balancing operations

**Alternative:** Develop a kernel module that directly calls `scx_bpf_pick_any_cpu_node()` with `SCX_PICK_IDLE_IN_NODE` flag to test the API behavior under the triggering conditions.
