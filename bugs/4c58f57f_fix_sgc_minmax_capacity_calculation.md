# Fix sgc->{min,max}_capacity calculation for SD_OVERLAP

- **Commit:** 4c58f57fa6e93318a0899f70d8b99fe6bac22ce8
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS, topology

## Bug Description

In SD_OVERLAP sched domains, the code incorrectly used the accumulated sum of CPU capacities (`capacity` variable) to compute the minimum and maximum per-CPU capacities (`min_capacity` and `max_capacity`). This caused the min/max capacity values to grow monotonically as CPUs were iterated, rather than tracking the actual minimum and maximum of individual CPU capacities. This resulted in incorrect capacity information used for scheduling decisions in overlapping domains.

## Root Cause

The bug occurred because the local variable `capacity` represents the cumulative sum of all CPU capacities in the group. However, the same accumulated value was incorrectly used in `min_capacity = min(capacity, min_capacity)` and `max_capacity = max(capacity, max_capacity)`, which should have been computed from individual CPU capacities, not the running sum. This logic error meant min/max values diverged from their correct values as the loop progressed.

## Fix Summary

The fix replaces the use of the accumulated `capacity` sum with individual CPU capacity values (`capacity_of(cpu)`) when computing min and max capacity. Each CPU's capacity is now correctly evaluated independently via `min_capacity = min(cpu_cap, min_capacity)` and `max_capacity = max(cpu_cap, max_capacity)`, while `capacity` continues to accumulate the sum as intended.

## Triggering Conditions

This bug is triggered during sched domain hierarchy update in the `update_group_capacity()` function when:
- The sched domain has the `SD_OVERLAP` flag set (overlapping domains where child groups don't span the current group)
- CPUs in the domain have different capacity values (asymmetric CPU capacities)
- The function iterates over multiple CPUs in the sched group span, accumulating capacity while incorrectly using the running sum for min/max calculations
- This typically occurs during system topology setup or CPU capacity updates, affecting domains like NUMA nodes or CPU clusters with heterogeneous cores

## Reproduce Strategy (kSTEP)

Setup an asymmetric topology with heterogeneous CPU capacities and overlapping domains, then trigger topology updates:
- Use 4+ CPUs (CPU 0 reserved for driver)
- Set different CPU capacities: `kstep_cpu_set_capacity(1, 1024)`, `kstep_cpu_set_capacity(2, 512)`, `kstep_cpu_set_capacity(3, 1024)`, `kstep_cpu_set_capacity(4, 512)`
- Configure overlapping domains via `kstep_topo_set_mc()` and `kstep_topo_set_pkg()` to create SD_OVERLAP scenarios
- Use `kstep_topo_apply()` to rebuild domains and trigger `update_group_capacity()` calls
- Monitor via callback functions to log sgc->min_capacity and sgc->max_capacity values during updates
- Bug manifests as monotonically increasing min/max values instead of actual per-CPU min/max (e.g., min_capacity > actual minimum CPU capacity)
- Verify fix by checking that min_capacity equals the smallest CPU capacity and max_capacity equals the largest
