# sched/deadline: Restore dl_server bandwidth on non-destructive root domain changes

- **Commit:** 41d4200b7103152468552ee50998cda914102049
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/topology.c
- **Subsystem:** Deadline scheduler, topology/root domain management

## Bug Description

When root domain non-destructive changes occur (e.g., modifying cpuset settings that affect only one root domain while others remain untouched), DEADLINE scheduler bandwidth accounting fails to properly restore dl_server contributions. dl_servers are deadline-based fair scheduling entities that consume DEADLINE bandwidth, but their bandwidth contributions are lost during non-destructive root domain changes, resulting in incorrect DEADLINE bandwidth accounting and potentially allowing overallocation of deadline bandwidth.

## Root Cause

After the introduction of dl_servers, the code only restored dl_server bandwidth contributions during destructive root domain changes (when domains are destroyed and CPUs are reattached). Non-destructive changes (where the topology is modified but an existing domain persists) explicitly clear the bandwidth but fail to restore the dl_server contributions that should be accounted for in that domain. The `dl_clear_root_domain()` function was missing logic to re-add dl_server bandwidth during these non-destructive changes.

## Fix Summary

The fix modifies `dl_clear_root_domain()` to iterate over all CPUs in the root domain and restore the bandwidth contribution of active dl_servers (fair_server entities) after clearing the total bandwidth. This ensures that dl_server bandwidth is properly accounted for both on destructive and non-destructive root domain changes.

## Triggering Conditions

The bug manifests during non-destructive root domain topology changes where:
- Multiple root domains exist (through cgroup cpuset partitioning)
- At least one root domain has active fair_server dl_servers consuming DEADLINE bandwidth  
- A cpuset modification triggers `partition_sched_domains_locked()` that affects one domain while others persist
- The persisting domain calls `dl_clear_root_domain()` which zeros total_bw but fails to restore dl_server contributions
- This results in incorrect bandwidth accounting where dl_server bandwidth is lost from the root domain's total

## Reproduce Strategy (kSTEP)

Use 3+ CPUs. Setup two cpuset partitions creating separate root domains. In `setup()`:
- `kstep_cgroup_create("partition1")` and `kstep_cgroup_create("partition2")`
- `kstep_cgroup_set_cpuset("partition1", "1-2")` and `kstep_cgroup_set_cpuset("partition2", "3")`
- Create CFS tasks and add to partitions to enable fair_servers
In `run()`:
- `kstep_tick_repeat(10)` to establish dl_server bandwidth usage  
- Read initial `rd->dl_bw.total_bw` for each domain via kernel logging
- Modify partition1 cpuset: `kstep_cgroup_set_cpuset("partition1", "1")` (non-destructive change)
- `kstep_tick()` to trigger topology update
- Compare final `total_bw` - it should show lost dl_server bandwidth in the persisting domain
Use `on_tick_begin()` to log bandwidth values and detect the missing dl_server contributions.
