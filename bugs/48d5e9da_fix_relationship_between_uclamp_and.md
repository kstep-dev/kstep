# sched/uclamp: Fix relationship between uclamp and migration margin

- **Commit:** 48d5e9daa8b767e75ed9421665b037a49ce4bc04
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS, uclamp

## Bug Description

The `fits_capacity()` function applies a 20% migration margin to determine if a task fits on a CPU. However, when uclamp constraints are active, this margin causes incorrect task placement decisions. For example, a task boosted to 1024 (or to capacity_orig of a target CPU) fails to fit on any CPU despite being valid under uclamp constraints, or ends up on a larger CPU than necessary. Similarly, irq pressure on the largest CPU can cause boosted tasks to appear as non-fitting even when they should be allowed.

## Root Cause

The bug occurs because `fits_capacity()` applies capacity pressure (via `capacity_of()`) and the migration margin uniformly to all checks. When uclamp is used, this logic conflates two different concepts: (1) whether a task's actual utilization fits, which should respect margin and pressure, and (2) whether a task's uclamp constraints are satisfiable on the CPU, which should use only the base capacity and ignore margin/pressure. This conflation causes tasks to be rejected or migrated incorrectly.

## Fix Summary

The fix introduces a new `util_fits_cpu()` function that properly separates the two concerns. It checks if the actual util fits using `fits_capacity()` (with margin and pressure), but for uclamp_min and uclamp_max constraints, it compares against `capacity_orig_of()` to ignore pressure, ensuring uclamp constraints are evaluated independent of transient system state. Thermal pressure is honored only for uclamp_min as it affects available OPP.

## Triggering Conditions

The bug triggers when tasks have active uclamp constraints (uclamp_min or uclamp_max set) and the scheduler uses `fits_capacity()` for task placement decisions. Key conditions include:
- A task with uclamp_min=1024 (or any high value) being evaluated on CPUs where `fits_capacity()` applies the 20% margin, causing rejection even when the CPU can satisfy the constraint
- A task with uclamp_max set to a medium CPU's capacity_orig being incorrectly placed on a big CPU instead of the intended medium CPU
- IRQ pressure on the largest CPU making uclamp_min=1024 tasks appear non-fitting when they should be allowed
- The race occurs during load balancing, task wakeup, or migration decisions where `fits_capacity()` checks conflict with uclamp constraint satisfaction
- Most commonly seen in energy-aware scheduling and big.LITTLE topologies where capacity differences amplify the margin effects

## Reproduce Strategy (kSTEP)

Use a big.LITTLE topology with 2 CPUs minimum. Create two tasks with different uclamp constraints:
- Setup: Configure asymmetric CPU capacities using `kstep_cpu_set_capacity()` (e.g., CPU1=1024, CPU2=512)
- Create task A with moderate util_avg but uclamp_min=1024 using cgroup controls via `kstep_cgroup_create()` and `kstep_cgroup_write()` to set uclamp.min
- Create task B with high util_avg but uclamp_max=512 (medium CPU capacity)
- Use `kstep_task_wakeup()` to trigger task placement decisions while monitoring CPU assignment
- Add IRQ pressure simulation on the big CPU using `kstep_write()` to procfs entries if available
- Monitor task placement in `on_tick_begin()` callback, checking if tasks land on appropriate CPUs
- Bug manifests as: task A rejected from all CPUs despite valid uclamp_min, or task B placed on big CPU instead of medium CPU
- Detection: Compare actual CPU assignment against expected placement based on pure uclamp constraint satisfaction
