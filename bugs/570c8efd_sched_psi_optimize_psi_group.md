# sched/psi: Optimize psi_group_change() cpu_clock() usage

- **Commit:** 570c8efd5eb79c3725ba439ce105ed1bedc5acd9
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

A previous fix (3840cbe24cf0) addressing aggregation race conditions introduced a performance regression on high context switch rate workloads like schbench. The fix caused cpu_clock() to be called once per group in the cgroup hierarchy during each task state change, creating significant overhead on systems with deep cgroup hierarchies, particularly on architectures where cpu_clock() is expensive.

## Root Cause

The previous fix moved the cpu_clock() call inside the group iteration loop, resulting in multiple cpu_clock() invocations per state change (one for each group traversal from leaf to root). This repeated clock reading also introduces potential inconsistency: if an update uses a timestamp from before the start of the update, the read side can get inconsistent results when extrapolating state to the current time.

## Fix Summary

The fix switches from per-group-per-cpu seqcount to a single per-cpu seqcount, allowing cpu_clock() to be called once at the start of all group updates and passed through as a parameter. The seqcount is held across all group changes via psi_write_begin()/psi_write_end(), maintaining consistency while eliminating redundant clock reads and ensuring timestamp consistency across the entire group hierarchy update.

## Triggering Conditions

The bug occurs during PSI (Pressure Stall Information) updates on systems with:
- Deep cgroup hierarchy (3+ nested levels from root to leaf cgroups)
- High-frequency task state changes (context switches, wakeups, sleeps)
- Tasks distributed across multiple cgroup levels in the hierarchy
- Architecture where cpu_clock() has significant overhead (e.g., certain ARM platforms)
- PSI enabled in kernel configuration

Each task state transition triggers psi_group_change() which traverses from leaf cgroup to root, calling cpu_clock() once per level in the buggy version. The performance regression becomes pronounced with >100 context switches/second across deep hierarchies.

## Reproduce Strategy (kSTEP)

Setup a 4-CPU system with nested cgroups and tasks generating frequent state changes:
- Use `kstep_cgroup_create()` to build hierarchy: root→"level1"→"level2"→"level3"  
- Create 6 tasks with `kstep_task_create()`, distribute across cgroup levels using `kstep_cgroup_add_task()`
- Pin tasks to CPUs 1-4 with `kstep_task_pin()` for controlled placement
- In `run()`, use `kstep_task_pause()`/`kstep_task_wakeup()` cycles to generate high-frequency state transitions
- Implement rapid sleep/wake patterns: `kstep_task_usleep(task, 100); kstep_tick(); kstep_task_wakeup()`
- Use `on_tick_begin()` callback to log PSI state changes and measure update latency
- Compare PSI update performance between buggy and fixed kernels by timing the update duration
- Look for >2x performance difference in PSI processing time on buggy kernel vs fixed
