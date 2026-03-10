# sched/core: Mitigate race cpus_share_cache()/update_top_cache_domain()

- **Commit:** 42dc938a590c96eeb429e1830123fef2366d9c80
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition exists in `cpus_share_cache()` when testing the same CPU (this_cpu == that_cpu). The unprotected access to the per-cpu variable `sd_llc_id` can race with `update_top_cache_domain()`, causing the function to return false when testing if a CPU shares a cache with itself. This race condition causes `ttwu_queue_cond()` to fail to detect that the processor ID matches the target CPU, triggering a warning from `ttwu_queue_wakelist()`.

## Root Cause

The `cpus_share_cache()` function accesses the per-cpu `sd_llc_id` variable without any synchronization mechanism. When CPU1 tests if it shares a cache with itself, CPU2 may simultaneously update the `sd_llc_id` value via `update_top_cache_domain()` during a domain partition. The timing of reads and writes can result in an inconsistent view of the cache domain, leading to an incorrect false return value for same-CPU comparisons.

## Fix Summary

The fix adds an early return of true when `this_cpu == that_cpu`, which is logically correct since a CPU always shares cache with itself. This eliminates the race condition for the same-CPU case by avoiding the unprotected per-cpu variable access when the comparison is between identical CPUs.

## Triggering Conditions

The bug triggers when `cpus_share_cache(cpu, cpu)` is called during a scheduling domain reconfiguration. The race occurs in the wake-up path (`ttwu_queue_cond`) when:
- CPU1 reads `sd_llc_id[CPUX]` the first time (gets 0)
- CPU2 concurrently executes `partition_sched_domains_locked()` → `update_top_cache_domain(CPUX)` and sets `sd_llc_id[CPUX] = CPUX`
- CPU1 reads `sd_llc_id[CPUX]` the second time (gets CPUX)
- The comparison fails (0 != CPUX), incorrectly returning false for same-CPU cache sharing
- `ttwu_queue_cond()` misses the `smp_processor_id() == cpu` optimization, triggering `ttwu_queue_wakelist()` warning

This requires concurrent domain reconfiguration (CPU hotplug, cgroup changes) and task wake-ups on the same CPU.

## Reproduce Strategy (kSTEP)

Use at least 3 CPUs (CPU 0 reserved for driver). Create a scenario that triggers domain reconfiguration concurrently with wake-ups:

1. **Setup**: Use `kstep_topo_init()` and `kstep_topo_apply()` to establish initial cache topology. Create multiple cgroups with `kstep_cgroup_create()` to enable domain partitioning.
2. **Trigger domain changes**: Repeatedly modify CPU topology or cgroup CPU sets using `kstep_cgroup_set_cpuset()` to trigger `partition_sched_domains_locked()`.
3. **Generate wake-ups**: Create tasks with `kstep_task_create()`, use `kstep_task_pause()` followed by rapid `kstep_task_wakeup()` calls to trigger `ttwu_queue_cond()`.
4. **Race timing**: Use `kstep_tick()` calls between topology changes and wake-ups to control timing. Use `on_tick_begin` callback to log `sd_llc_id` values.
5. **Detection**: Monitor for warnings from `ttwu_queue_wakelist()` in kernel logs. Check if `cpus_share_cache(cpu, cpu)` ever returns false by adding custom logging in the scheduler.
