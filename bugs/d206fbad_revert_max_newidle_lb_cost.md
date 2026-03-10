# sched/fair: Revert max_newidle_lb_cost bump

- **Commit:** d206fbad9328ddb68ebabd7cf7413392acd38081
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

A previous commit (155213a2aed4) introduced aggressive bumping of `max_newidle_lb_cost` when newidle load balancing fails, intended to skip ineffective balance attempts. However, this caused significant performance regressions on real-world database workloads, with reports of up to 6% regression on SpecJBB and similar degradations on other benchmarks. The issue is that the artificially inflated cost causes the scheduler to skip too many newidle balance opportunities, leading to suboptimal scheduling decisions and overall performance degradation.

## Root Cause

The previous implementation artificially multiplied the domain cost by 1.5x (`(3 * sd->max_newidle_lb_cost) / 2`) when no task was successfully pulled during newidle balancing, and capped it at `sysctl_sched_migration_cost + 200`. This aggressive growth of the cost threshold caused `max_newidle_lb_cost` to become inflated too quickly, making the system skip legitimate load balancing opportunities. As a result, CPUs remain idle longer than necessary while tasks are pending elsewhere, degrading overall system performance.

## Fix Summary

The fix removes the artificial cost bumping logic and the artificial cap, allowing `max_newidle_lb_cost` to track the actual measured cost of domain balancing. The cost is updated directly based on real execution time without inflation, with only a natural decay mechanism to prevent the metric from becoming stale. This restores proper load balancing behavior while maintaining the original intent of avoiding pathological repeated balancing attempts.

## Triggering Conditions

The bug occurs when newidle load balancing repeatedly fails across scheduling domains, causing `max_newidle_lb_cost` to grow artificially large. Triggering requires: (1) Multi-domain CPU topology where newidle balancing operates across domain boundaries, (2) Load imbalance patterns that cause `sched_balance_newidle()` to fail pulling tasks due to affinity restrictions, task priorities, or timing issues, (3) Repeated failed balance attempts that trigger the artificial cost bumping logic (`(3 * sd->max_newidle_lb_cost) / 2`), and (4) Subsequent legitimate balancing opportunities where the inflated cost threshold causes the scheduler to skip newidle balancing entirely. The performance degradation manifests when CPUs remain idle despite available work on other CPUs because the cost check prevents load balancing attempts.

## Reproduce Strategy (kSTEP)

Set up 4+ CPUs with multi-cluster topology using `kstep_topo_set_cls()` to create scheduling domains. Create task load imbalance by pinning multiple tasks to specific clusters using `kstep_task_pin()`, then create conditions that cause newidle balancing failures (e.g., tasks with restricted affinity or different priorities). Use `kstep_tick_repeat()` to advance time and trigger repeated newidle balance attempts. Monitor `max_newidle_lb_cost` values and balance decisions through `on_sched_balance_selected` callbacks. Create a scenario where legitimate balancing should occur (idle CPU with available tasks elsewhere), then verify that inflated costs prevent balancing. Compare behavior before/after the artificial cost bumping - the buggy version should show skipped balance opportunities and higher latency for task migration, while the fixed version should perform timely load balancing.
