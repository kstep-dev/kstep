# Revert "cpufreq: schedutil: Move max CPU capacity to sugov_policy"

- **Commit:** cdcc5ef26b39c3d02d4e69c0352b007ebe438a22
- **Affected file(s):** kernel/sched/cpufreq_schedutil.c
- **Subsystem:** CPUfreq

## Bug Description

A previous commit moved the max CPU capacity field from per-CPU to per-policy level, which caused a 50ms (20%) latency increase when migrating tasks requiring full CPU utilization from a LITTLE CPU to a big CPU on heterogeneous ARM systems (e.g., Pixel 6). The bug manifests because heterogeneous CPU systems have CPUs with different maximum capacities within the same frequency domain, but the per-policy approach cannot represent this variation, leading to incorrect frequency scaling decisions.

## Root Cause

The bug occurs because max CPU capacity cannot be universally represented at the policy level when a frequency domain contains CPUs with heterogeneous capacities. In big.LITTLE systems, LITTLE and big cores have different capacity values, but storing a single `max` per policy means the governor uses the same capacity value for all CPUs in the domain. This causes improper utilization-to-frequency mapping and prevents the scheduler from making optimal frequency decisions for task migration, resulting in delayed CPU scaling and increased latency.

## Fix Summary

The fix reverts the problematic change by moving max CPU capacity back to per-CPU level (`sugov_cpu.max`) and computing it individually for each CPU via `arch_scale_cpu_capacity()` in `sugov_get_util()`. All frequency calculation code is updated to use the per-CPU capacity value, allowing the governor to correctly account for capacity heterogeneity and restore original latency performance.

## Triggering Conditions

The bug requires a heterogeneous CPU system (big.LITTLE architecture) where CPUs with different capacities share the same frequency domain. Specifically:
- Multiple CPUs in the same cpufreq policy with different `arch_scale_cpu_capacity()` values
- A high-utilization task initially placed on a LITTLE core (low capacity CPU)
- Task migration from LITTLE to big core triggered by load balancer or scheduler
- Schedutil governor active and `sugov_get_util()` called during frequency scaling
- The per-policy max capacity incorrectly represents all CPUs, causing wrong util-to-freq mapping
- Results in delayed frequency scaling when task moves to higher-capacity CPU

## Reproduce Strategy (kSTEP)

Setup heterogeneous topology with 2+ CPUs, capacity differences in same freq domain:
- Use `kstep_cpu_set_capacity()` to create LITTLE (CPU 1, capacity 512) and big (CPU 2, capacity 1024)
- Set topology so CPUs 1-2 share frequency domain via `kstep_topo_set_cls()`
- Create high-utilization task with `kstep_task_create()` and pin to LITTLE core via `kstep_task_pin(task, 1, 1)`
- Run task until it reaches full utilization on LITTLE core with `kstep_tick_repeat()`
- Migrate task to big core with `kstep_task_pin(task, 2, 2)` and trigger frequency update
- Use `on_tick_begin()` callback to monitor frequency scaling decisions via schedutil
- Check if frequency scaling is delayed/incorrect by logging CPU frequency changes
- Compare behavior before/after the problematic commit to detect 50ms latency increase
