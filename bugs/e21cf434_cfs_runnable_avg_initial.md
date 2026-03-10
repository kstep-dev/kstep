# sched/cfs: change initial value of runnable_avg

- **Commit:** e21cf43406a190adfcc4bfe592768066fb3aaa9b
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

A performance regression was introduced by commit 070f5e860ee2 where `runnable_avg` was initialized to the maximum CPU capacity value. For newly forked tasks that turn out to be short-lived, this causes the CPU group to be wrongly classified as overloaded, preventing the scheduler from aggressively pulling and balancing tasks across CPUs. This results in measurable performance degradation on workloads like the reaim benchmark.

## Root Cause

The `post_init_entity_util_avg()` function initialized `sa->runnable_avg` to `cpu_scale` (the maximum value), which does not reflect the actual running state of a newly created task. Since `runnable_avg` is used to classify CPU group load status, setting it to the maximum value causes false positives in overload detection, leading to suboptimal load balancing decisions even for short tasks with minimal actual runtime.

## Fix Summary

Change the initialization of `sa->runnable_avg` from `cpu_scale` to `sa->util_avg`, reflecting that a newly forked task has no waiting time. This prevents the false overload classification and restores proper load balancing behavior for short-lived tasks.

## Triggering Conditions

The bug is triggered during task creation via the CFS scheduler's load average initialization:
- A new task is forked and enters the CFS scheduler class
- `post_init_entity_util_avg()` is called to initialize task's scheduling averages
- The function sets `sa->runnable_avg` to `cpu_scale` (1024 on standard systems) regardless of actual task behavior
- This artificially high `runnable_avg` causes CPU groups containing these tasks to appear overloaded
- Load balancing logic in `group_is_overloaded()` relies on `runnable_avg` to detect overload conditions
- False overload detection prevents aggressive task pulling, reducing load balancing effectiveness
- The impact is most visible with short-lived tasks that exit before their `runnable_avg` decays naturally
- Multi-CPU systems with uneven load distribution are particularly affected as balancing becomes suboptimal

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved, CPUs 1-2 for testing):
1. In `setup()`: Create multiple short-lived tasks using `kstep_task_create()`
2. In `run()`: 
   - Pin initial load to CPU 1 using `kstep_task_pin(task, 1, 1)`  
   - Create burst of new tasks with `kstep_task_fork(parent, n)` - these trigger `post_init_entity_util_avg()`
   - Let new tasks migrate naturally without explicit pinning
   - Use `kstep_tick_repeat(5)` to allow minimal runtime before tasks exit
3. Use `on_sched_balance_selected()` callback with `kstep_output_balance()` to log load balancing decisions
4. Use `on_tick_begin()` callback with `kstep_output_nr_running()` to track task distribution
5. Detection: Compare task distribution across CPUs - buggy kernel shows poor balancing due to false overload signals from high initial `runnable_avg` values. Fixed kernel shows better task spreading as `runnable_avg` reflects actual utilization.
