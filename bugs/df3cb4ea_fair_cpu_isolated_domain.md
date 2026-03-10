# sched/fair: Fix wrong cpu selecting from isolated domain

- **Commit:** df3cb4ea1fb63ff326488efd671ba3c39034255e
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair scheduling)

## Bug Description

Tasks with full cpumask were occasionally being migrated to isolated CPUs in production environments when using the `isolcpus=domain` boot parameter. This violated the isolation policy and caused tasks to run on CPUs they should not have been assigned to. The issue was reproducible on a 31-CPU hyperthreading system by creating a cpuset with full affinity and observing threads migrating to isolated CPUs 16-17.

## Root Cause

The `select_idle_smt()` function was only checking if a candidate CPU was in the task's cpumask but was not verifying that the CPU was within the valid sched_domain span. This allowed the scheduler to select CPUs (particularly isolated ones) that fell outside the current scheduling domain, bypassing domain-level isolation constraints.

## Fix Summary

The fix adds the `struct sched_domain *sd` parameter to `select_idle_smt()` and adds an additional check to ensure a candidate CPU is within both the task's cpumask AND the sched_domain span. This ensures that CPU selection respects domain-level isolation constraints and prevents tasks from being migrated to isolated CPUs that should be excluded from the search.

## Triggering Conditions

This bug occurs during idle CPU selection in the CFS scheduler when `select_idle_smt()` is called within `select_idle_siblings()`. Key conditions:
- SMT topology where some CPUs are isolated from the current scheduling domain (e.g., `isolcpus=domain` boot parameter)
- Tasks with broad CPU affinity that includes both domain and isolated CPUs
- The target CPU for idle selection has SMT siblings that are isolated (outside the sched_domain span)
- The scheduler attempts to find idle SMT siblings during load balancing or task wakeup
- Race condition where `select_idle_smt()` checks only task affinity but not domain span constraints

## Reproduce Strategy (kSTEP)

Configure 4+ CPUs with SMT topology where some CPUs are excluded from the main scheduling domain:
- Setup: Use `kstep_topo_init()` and `kstep_topo_set_smt()` to create SMT pairs (e.g., [1,2], [3,4])
- Create scheduling domains that exclude certain CPUs to simulate isolation
- Use `kstep_task_create()` to create tasks with broad affinity via `kstep_task_pin(task, 1, 4)`
- Trigger idle CPU selection by creating load imbalance: pin multiple tasks to one CPU, leave SMT siblings idle
- Use `kstep_tick_repeat()` to advance the scheduler and trigger `select_idle_siblings()` calls
- Monitor with `on_sched_balance_selected()` callback to detect if isolated CPUs are incorrectly selected
- Verify bug by checking if tasks appear on CPUs outside the intended scheduling domain span
- Log CPU assignments and domain spans to detect domain boundary violations
