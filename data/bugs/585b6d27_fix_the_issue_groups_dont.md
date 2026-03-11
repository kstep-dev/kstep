# sched/topology: fix the issue groups don't span domain->span for NUMA diameter > 2

- **Commit:** 585b6d2723dc927ebc4ad884c4e879e4da8bc21f
- **Affected file(s):** kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

When building sched_domains on NUMA systems with diameter > 2, the scheduler creates sched_groups that span outside of their parent sched_domain. This causes `load_balance()` to operate on incorrect avg_load and group_type values from groups outside the domain, and causes `select_task_rq_fair()` to select idle CPUs outside the intended sched_domain span. This incorrect behavior manifests as scheduling decisions based on screwed load balancing information.

## Root Cause

When building overlap sched_groups, the code uses a sibling's child sched_domain to create groups. However, with NUMA diameter > 2, this child domain can span beyond the current domain being built. For example, when node 0 builds domain2 (span 0-5) and includes node 2's child domain (domain1 covering nodes 2-3), node 3 extends outside domain2's span, violating the invariant that all groups must be subsets of the domain span.

## Fix Summary

The fix introduces `find_descended_sibling()` to locate the proper descendant level of a sibling's domain hierarchy that fits within the current domain span. Instead of blindly using the sibling's direct child, the code now descends through the domain tree until finding a domain whose child won't overflow the parent domain, ensuring all groups properly span within their containing domain.

## Triggering Conditions

This bug triggers during scheduler domain construction in `build_overlap_sched_groups()` when:
- NUMA topology has diameter > 2 (shortest path between furthest nodes spans 3+ hops)
- The topology builder processes a sibling domain whose child spans beyond the current domain
- Example: 4-node linear topology (0-1-2-3) where domain2 of node0 (span 0-2) incorrectly includes node2's child domain1 (span 2-3), causing node3 to extend outside domain2's span
- This creates sched_groups that violate the invariant: group span ⊄ domain span
- Manifests during load balancing and task placement as decisions based on incorrect load information from out-of-domain CPUs

## Reproduce Strategy (kSTEP)

Setup a 4-node linear NUMA topology with diameter=3 using 8 CPUs (nodes 0-1-2-3, 2 CPUs per node):
- Use `kstep_topo_init()` and `kstep_topo_set_node()` to create 4 NUMA nodes with CPUs 1-2, 3-4, 5-6, 7-8
- Configure NUMA distances to create diameter > 2: close neighbors (distance 12), far neighbors (distance 20+)
- Apply topology with `kstep_topo_apply()` to trigger domain building during kernel boot
- Use `kstep_topo_print()` to verify domain construction and identify incorrect group spans
- Monitor during `on_sched_balance_begin()` callback for load balancing operations that access out-of-domain CPUs
- Check scheduler debug output with `kstep_print_sched_debug()` to detect groups spanning outside their domain
- Create tasks pinned to different nodes and observe incorrect load balancing decisions via `kstep_output_balance()`
