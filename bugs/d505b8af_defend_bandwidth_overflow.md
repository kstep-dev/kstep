# sched: Defend cfs and rt bandwidth quota against overflow

- **Commit:** d505b8af58912ae1e1a211fabc9995b19bd40828
- **Affected file(s):** kernel/sched/core.c, kernel/sched/rt.c, kernel/sched/sched.h
- **Subsystem:** CFS and RT bandwidth management

## Bug Description

When users write very large numbers into cpu.cfs_quota_us or cpu.rt_runtime_us cgroup interfaces, integer overflow can occur during the to_ratio() shifts used in bandwidth schedulability checks. This can result in incorrect quota calculations and potentially cause the scheduler to violate bandwidth constraints. The bug manifests when users attempt to set quota values that are large enough to overflow when shifted during bandwidth computations.

## Root Cause

The kernel did not validate quotas against an upper bound before using them in to_ratio() operations, which involves left-shifting by BW_SHIFT (20 bits). Without bounds checking, sufficiently large quota values would overflow during these shifts, causing the bandwidth ratio calculations to wrap around and produce incorrect results. The min_cfs_quota_period constraint was insufficient to prevent overflow, necessitating an explicit cap (MAX_BW).

## Fix Summary

The fix introduces a MAX_BW constant (derived from 64 - BW_SHIFT bits) to cap both cfs_quota_us and rt_runtime_us values. Additional validation checks are added in tg_set_cfs_bandwidth() and tg_set_rt_bandwidth() to reject quota values exceeding max_cfs_runtime and max_rt_runtime respectively, preventing overflow during bandwidth shift operations.

## Triggering Conditions

The bug triggers when writing quota values >= MAX_BW * NSEC_PER_USEC (~203 days for CFS, or ~4 hours for RT in microseconds) into cgroup cpu.cfs_quota_us or cpu.rt_runtime_us interfaces. The overflow occurs in to_ratio() during schedulability checks (__cfs_schedulable() or __rt_schedulable()), where quota values are left-shifted by BW_SHIFT (20 bits) for bandwidth ratio calculations. Without upper bounds validation, sufficiently large quota values wrap around during these shifts, producing incorrect bandwidth ratios that can violate scheduling constraints and cause bandwidth accounting errors.

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. In setup(), create a cgroup with kstep_cgroup_create("testgroup"). In run(), attempt to write a quota value exceeding MAX_BW limit using kstep_cgroup_write("testgroup", "cpu.cfs_quota_us", "%llu", overflow_quota) where overflow_quota = (MAX_BW + 1) * 1000 (microseconds). The buggy kernel will accept this value and proceed with schedulability checks, while the fixed kernel should reject it with -EINVAL. Monitor via kstep_fail() when the write succeeds (indicating overflow vulnerability) or kstep_pass() when it properly fails. For RT bandwidth, use cpu.rt_runtime_us with a value exceeding max_rt_runtime. Detection relies on whether the cgroup write operation succeeds or fails appropriately.
