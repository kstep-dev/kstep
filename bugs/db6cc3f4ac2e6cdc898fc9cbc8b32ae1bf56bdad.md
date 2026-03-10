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
