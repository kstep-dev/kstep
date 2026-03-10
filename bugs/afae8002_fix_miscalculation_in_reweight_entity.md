# sched/eevdf: Fix miscalculation in reweight_entity() when se is not curr

- **Commit:** afae8002b4fd3560c8f5f1567f3c3202c30a70fa
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler) / EEVDF

## Bug Description

When reweighting a non-current task entity, the vruntime calculations in `reweight_eevdf()` produce incorrect results. The entity's average vruntime (V) used in the calculation is stale because it's computed after the entity is dequeued from the runqueue, which changes the cfs_rq's average vruntime. This leads to incorrect deadline and vslice calculations, causing the task to be scheduled at the wrong position.

## Root Cause

The original code dequeued the entity first, then called `reweight_eevdf()` which internally computed `avruntime = avg_vruntime(cfs_rq)`. However, dequeuing the entity modifies the cfs_rq's average vruntime, so the value used in the reweighting math no longer reflects the vruntime state at the time of the reweight decision. This breaks the correctness of the vruntime adjustment formula.

## Fix Summary

The fix captures the original average vruntime before dequeuing the entity and passes it as a parameter to `reweight_eevdf()`. This ensures the vruntime calculations use the correct value from before the dequeue operation, preserving the mathematical correctness of the deadline and vslice adjustments.

## Triggering Conditions

- A non-current task entity (`se != cfs_rq->curr`) must exist on the runqueue
- The entity needs to undergo reweighting (nice value change, cgroup weight modification, etc.)
- Multiple tasks must be present so that dequeuing one task meaningfully changes `avg_vruntime(cfs_rq)`
- The timing must hit the `reweight_entity()` path where the entity gets dequeued before `reweight_eevdf()` is called
- The entity should have non-zero lag to make the incorrect vruntime calculation observable

## Reproduce Strategy (kSTEP)

- Use 2+ CPUs (CPU 0 reserved for driver)
- **Setup:** Create 3 tasks with different weights via `kstep_task_create()` and `kstep_task_set_prio()`
- **Setup cgroups:** Use `kstep_cgroup_create()` and `kstep_cgroup_set_weight()` for dynamic reweighting
- **Run sequence:** 
  1. Wake all tasks with `kstep_task_wakeup()`, run with `kstep_tick_repeat()` to establish vruntime spread
  2. Ensure one task becomes non-current (paused or lower priority)  
  3. Trigger reweight via `kstep_cgroup_set_weight()` on the non-current task
- **Detection:** Use `on_tick_end()` callback to log entity vruntime, deadline, and cfs_rq avg_vruntime before/after reweight
- **Verification:** Compare computed vs actual vlag/deadline values to detect miscalculation
