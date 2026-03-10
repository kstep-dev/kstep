# mm: memory-tiering: fix PGPROMOTE_CANDIDATE counting

- **Commit:** 337135e6124b6d37d7ef1cd5a6c0b9681938c5ee
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NUMA balancing / Memory tiering

## Bug Description

On systems with memory tiering (fast DRAM and slow NVDIMM), promotion statistics become inconsistent: `pgpromote_success` significantly exceeds `pgpromote_candidate`, which is logically impossible. The bug occurs when the workload changes and free memory becomes available on the destination node—pages are promoted by bypassing the rate limit check, but these pages are only counted in `PGPROMOTE_SUCCESS`, not in `PGPROMOTE_CANDIDATE`, creating a confusing and incorrect statistic.

## Root Cause

The `should_numa_migrate_memory()` function in NUMA balancing has a fast path triggered by `pgdat_free_space_enough()`. When this condition is met (indicating a workload change and availability of free space), the function returns `true` immediately, promoting pages without going through the rate-limit check that increments `PGPROMOTE_CANDIDATE`. As a result, these promoted pages are counted only in `PGPROMOTE_SUCCESS`, not in candidate statistics, causing the inconsistency.

## Fix Summary

The fix introduces a new counter `PGPROMOTE_CANDIDATE_NRL` (not rate limited) to account for promotion candidates that bypass the rate limit. When `pgdat_free_space_enough()` is satisfied, the number of pages being promoted is now explicitly counted via `mod_node_page_state()`, ensuring that all promoted pages are reflected in the candidate statistics, fixing the confusing discrepancy.

## Triggering Conditions

The bug is triggered when NUMA balancing is enabled and the system has multiple NUMA nodes with different memory tiers (e.g., DRAM + NVDIMM). The key sequence: (1) Heavy memory workload fills fast-tier nodes causing pages to be demoted to slow tier, (2) Original memory workload suddenly terminates or reduces significantly, freeing up space on fast-tier nodes, (3) `pgdat_free_space_enough()` condition is met for destination node, (4) `should_numa_migrate_memory()` bypasses rate limit and returns `true` immediately, (5) Pages get promoted via the fast path, counted only in `PGPROMOTE_SUCCESS` but not in `PGPROMOTE_CANDIDATE`. The race occurs in the transition phase when workload pressure decreases but the scheduler still has pages to promote from slow to fast memory.

## Reproduce Strategy (kSTEP)

Setup a multi-node topology with at least 3 CPUs (CPU 0 reserved for driver). Create memory pressure tasks using `kstep_task_create()` and `kstep_task_fork()` to simulate the memhog workload. Use `kstep_sysctl_write("kernel/numa_balancing", "2")` to enable NUMA balancing. Pin high-memory tasks to specific nodes using cgroups with `kstep_cgroup_create()` and `kstep_cgroup_add_task()` to force demotion. After running `kstep_tick_repeat()` for sufficient time to establish memory pressure, abruptly terminate the memory-heavy tasks with `kstep_task_pause()` to trigger the `pgdat_free_space_enough()` condition. Monitor via `on_tick_end()` callback to access `/proc/vmstat` counters using `kstep_write()` to parse pgpromote statistics. The bug manifests when `pgpromote_success > pgpromote_candidate` after the workload transition, indicating the accounting inconsistency.
