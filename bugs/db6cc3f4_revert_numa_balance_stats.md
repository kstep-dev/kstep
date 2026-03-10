# Revert "sched/numa: add statistics of numa balance task"

- **Commit:** db6cc3f4ac2e6cdc898fc9cbc8b32ae1bf56bdad
- **Affected file(s):** kernel/sched/core.c, kernel/sched/debug.c
- **Subsystem:** NUMA balancing

## Bug Description

A NULL pointer dereference occurs during NUMA task migration when a task exits after being selected as a migration candidate but before the actual migration completes. The crash happens in `count_memcg_event_mm(p->mm, NUMA_TASK_SWAP)` when p->mm is NULL. This is a classic race condition where the task's memory descriptor is freed on one CPU while another CPU is attempting to access it during migration.

## Root Cause

The race condition occurs between task_numa_migrate() and task exit: task_numa_compare() selects a task candidate and stores it in env->best_task. Concurrently, the selected task may exit and set its p->mm to NULL via exit_mm(). When migrate_swap_stop() later attempts to record NUMA statistics by accessing p->mm, it dereferences a NULL pointer. The original patch added statistics tracking without proper synchronization (missing task_lock() and PF_EXITING flag checks).

## Fix Summary

The commit reverts the NUMA balance statistics addition, removing all calls to __schedstat_inc(p->stats.numa_task_swapped), count_vm_numa_event(), and count_memcg_event_mm() from the migration code paths. The patch preserves tracepoint-based monitoring as an alternative for observing NUMA migration activity.

## Triggering Conditions

The bug requires a precise race between NUMA migration and task exit. CPU0 must be executing `task_numa_migrate()` -> `task_numa_find_cpu()` -> `task_numa_compare()` to select a migration candidate task and store it in `env->best_task`. Simultaneously, CPU1 must cause the selected task to exit by calling `exit_signals()` (setting PF_EXITING flag) followed by `exit_mm()` (setting `p->mm = NULL`). The timing window is between candidate selection and the later call to `migrate_swap_stop()` -> `__migrate_swap_task()` -> `count_memcg_event_mm(p->mm, NUMA_TASK_SWAP)`. This requires NUMA balancing enabled, multi-NUMA topology, memory pressure triggering migration, and sufficient task churn to hit the narrow race window.

## Reproduce Strategy (kSTEP)

Requires 4+ CPUs across 2 NUMA nodes. In `setup()`: use `kstep_topo_set_node()` to create NUMA topology, `kstep_cgroup_create()` for memory pressure, create 10+ short-lived tasks with `kstep_task_create()`. In `run()`: pin tasks to different NUMA nodes with `kstep_task_pin()`, start memory-intensive workload to trigger NUMA migration, use `kstep_task_fork()` and rapid `kstep_task_pause()`/task exit to create concurrent task churn. Use `on_sched_softirq_begin()` callback to detect when NUMA balancer runs and `on_tick_end()` to log task states. Monitor for NULL pointer dereference in kernel logs or use KASAN to detect the race. The bug manifests as a crash in `count_memcg_event_mm()` when accessing freed `p->mm`.
