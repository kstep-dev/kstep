# kthread: Honour kthreads preferred affinity after cpuset changes

- **Commit:** e894f633980804a528a2d6996c4ea651df631632
- **Affected file(s):** kernel/sched/isolation.c
- **Subsystem:** core (housekeeping/CPU isolation)

## Bug Description

When cpuset isolated partitions are created, updated, or deleted, unbound kthreads lose their individual affinity preferences and get indiscriminately bound to all non-isolated CPUs. This breaks per-node kthreads like kswapd, which should maintain affinity to their specific node even when that node has some isolated CPUs. As a result, kswapd's node affinity gets corrupted if any CPU in its node is not isolated.

## Root Cause

The `housekeeping_update()` function updates the housekeeping cpumask when isolation partitions change, but it does not notify the kthread affinity management code to reapply kthreads' preferred affinity constraints. This leaves kthread affinity preferences in an inconsistent state relative to the new isolation configuration.

## Fix Summary

The fix adds a call to `kthreads_update_housekeeping()` in the `housekeeping_update()` sequence to ensure that the consolidated kthread affinity management code reapplies kthreads' preferred affinity constraints after isolation mask changes, preventing affinity preferences from being lost.

## Triggering Conditions

The bug occurs when cpuset isolated partitions are created, updated, or deleted while unbound kthreads (like kswapd, per-node kthreads) have preferred affinity constraints. The `housekeeping_update()` function updates the housekeeping cpumask based on CPU isolation changes, but fails to notify kthread affinity management to reapply preferred affinities. This causes per-node kthreads to lose their node-specific affinity and become globally affine to all non-isolated CPUs, breaking their intended locality.

## Reproduce Strategy (kSTEP)

Use at least 4 CPUs (needs NUMA topology). Set up multi-node topology with `kstep_topo_set_node()` where each node has 2 CPUs. Create kthreads with preferred node affinity using `kstep_kthread_create()` followed by setting preferred affinity. Create cpuset partitions with `kstep_cgroup_create()` and configure isolation via cpuset files with `kstep_cgroup_write()`. Update cpuset configurations to trigger housekeeping mask changes. Use callbacks to monitor kthread affinity changes and verify that per-node kthreads maintain their node affinity instead of becoming globally affine to all non-isolated CPUs.
