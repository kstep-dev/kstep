# sched/fair: Fixes for capacity inversion detection

- **Commit:** da07d2f9c153e457e845d4dcfdd13568d71d18a4
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Capacity-aware scheduling)

## Bug Description

The capacity inversion detection code in `update_cpu_capacity()` had two critical issues: it lacked proper RCU protection when traversing performance domains, and it incorrectly compared a domain against itself during capacity inversion checks. The missing RCU lock could lead to use-after-free or memory safety violations when accessing the perf domain list, while the self-comparison logic error could produce incorrect capacity inversion results.

## Root Cause

The original code called `rcu_dereference()` to access the performance domain list without holding `rcu_read_lock()`, violating RCU synchronization requirements. Additionally, the capacity inversion check loop failed to skip the current CPU's own performance domain before comparing capacities, causing logically invalid comparisons against itself.

## Fix Summary

The fix adds `rcu_read_lock()` and `rcu_read_unlock()` guards around the perf domain traversal to provide proper RCU protection. It also adds a check to skip the current domain's span during the loop (`if (cpumask_test_cpu(cpu_of(rq), pd_span)) continue`), and changes the gating condition from `static_branch_unlikely(&sched_asym_cpucapacity)` to the more appropriate `sched_energy_enabled()`.

## Triggering Conditions

The bug is triggered when the scheduler calls `update_cpu_capacity()` during load balancing on systems with energy-aware scheduling enabled and asymmetric CPU capacities. Specifically, the system must have multiple performance domains with different capacity_orig values, and thermal pressure must be present to cause capacity reduction. The RCU violation occurs when the performance domain list is accessed without proper RCU protection during the capacity inversion detection loop. The self-comparison bug manifests when the current CPU's own performance domain is included in the inversion check loop, leading to incorrect capacity inversion state calculations.

## Reproduce Strategy (kSTEP)

Use 4+ CPUs with kSTEP to create asymmetric capacity topology. In `setup()`, call `kstep_cpu_set_capacity()` to set different capacities (e.g., CPU 1-2 at full scale, CPU 3-4 at half scale), then use `kstep_topo_set_cls()` to group CPUs into separate performance domains. Create tasks with `kstep_task_create()` and pin them to different CPUs using `kstep_task_pin()` to create load imbalance. In `run()`, trigger multiple load balancing rounds with `kstep_tick_repeat()` to invoke `update_cpu_capacity()`. Use `on_sched_balance_begin()` callback to monitor when load balancing occurs. The RCU bug may manifest as memory safety violations or kernel warnings, while the self-comparison bug can be detected by checking if `cpu_capacity_inverted` flag shows incorrect inversion states when domains compare against themselves.
