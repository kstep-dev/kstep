# sched/fair: Fix task utilization accountability in compute_energy()

- **Commit:** 0372e1cf70c28de6babcba38ef97b6ae3400b101
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair Scheduler), Energy-Aware Scheduling

## Bug Description

The `compute_energy()` function uses `cpu_util_next()` to estimate CPU utilization for energy calculations, but this function inconsistently accounts for task utilization depending on whether the CPU is idle or busy. On idle CPUs, the task contributes `_task_util_est` (estimated), while on busy CPUs it contributes `task_util` (actual). Since `_task_util_est > task_util` for waking tasks, this causes busy CPUs to appear more energy-efficient than they actually are, biasing task placement away from idle CPUs and distorting energy-based scheduling decisions.

## Root Cause

The `cpu_util_next()` function computes `max(cpu_util + task_util, cpu_util_est + _task_util_est)`, which means the task's contribution to energy is not consistent across CPUs: it depends on whether `cpu_util` or `cpu_util_est` dominates. This inconsistency is systematic—idle CPUs naturally stay idle because they look more expensive for energy-aware task placement, while busy CPUs get systematically favored.

## Fix Summary

The fix separates the task utilization accounting into two components: for energy cost calculation (ENERGY_UTIL), always use `max(task_util, _task_util_est)` as the task's contribution; for frequency selection (FREQUENCY_UTIL), continue using `cpu_util_next()` to determine the actual peak utilization in the performance domain. This ensures fair comparison of energy deltas across CPUs while still correctly estimating the OPP that would be selected.

## Triggering Conditions

This bug manifests in Energy-Aware Scheduling (EAS) during `find_energy_efficient_cpu()` when selecting placement for waking tasks. The trigger requires: (1) EAS-enabled system with asymmetric CPU capacities, (2) mixed CPU utilization where some CPUs are mostly idle (`cpu_util ≈ 0`) and others are busy (`cpu_util > _task_util_est`), (3) a waking task with `_task_util_est > task_util` (typical for tasks that have been sleeping), and (4) energy comparison between CPUs in `compute_energy()`. The inconsistent task contribution (`_task_util_est` on idle CPUs vs `task_util` on busy CPUs) systematically biases energy calculations, making busy CPUs appear more efficient and starving idle CPUs of new tasks.

## Reproduce Strategy (kSTEP)

Use 4+ CPUs with asymmetric capacities to enable EAS: `kstep_cpu_set_capacity()` to create big.LITTLE topology. Set up performance domains with `kstep_topo_set_cls()`. Create background tasks on CPUs 1-2 to make them busy, leave CPU 3 mostly idle. Use `kstep_task_create()` + `kstep_task_pin()` for background load. Create a target task, let it run briefly to establish `task_util`, then `kstep_task_pause()` to make `_task_util_est > task_util`. Monitor energy-based placement decisions by tracking where `kstep_task_wakeup()` places the target task using `on_tick_begin` callback with `kstep_output_curr_task()`. Bug manifests as systematic avoidance of idle CPU 3 despite it being energy-optimal. Verify fix by comparing placement patterns before/after the patch.
