# Fix initial util_avg calculation

- **Commit:** 72bffbf57c5247ac6146d1103ef42e9f8d094bc8
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The initial util_avg calculation for newly created tasks uses an inconsistent unit scaling when mixing `se->load.weight` (which is scaled-up by 1024 on 64-bit systems) with `cfs_rq->avg.load_avg` (which uses true task weight). On CONFIG_64BIT systems, this causes the calculated util_avg to be inflated by 1024 times before being capped, resulting in incorrect scheduler decisions about task utilization that can affect scheduling behavior and load balancing accuracy.

## Root Cause

The code mixed two load weight representations with different scaling: `se->load.weight` is a scaled-up load (1024x on 64-bit), while `cfs_rq->avg.load_avg` represents true task weight without scaling. This unit mismatch caused the numerator to be 1024x larger than intended when divided by the denominator on 64-bit systems. Although subsequent capping prevented wild values, the calculation logic itself was incorrect and could lead to suboptimal initial utilization estimates.

## Fix Summary

Replace `se->load.weight` with `se_weight(se)` in the util_avg calculation to ensure consistent unit scaling. The `se_weight(se)` helper extracts the true task weight without the 1024x scaling factor, making the calculation semantically correct and unit-consistent with the denominator.

## Triggering Conditions

The bug occurs on CONFIG_64BIT systems during task creation when the scheduler calculates initial util_avg values. Key conditions:
- Active CFS runqueue with non-zero `cfs_rq->avg.load_avg` and `cfs_rq->avg.util_avg` from existing tasks
- Creation of a new task that triggers `init_entity_runnable_average()` and subsequent util_avg calculation
- The code path uses `se->load.weight` (1024x scaled) instead of true task weight in the formula: 
  `util_avg = cfs_rq->avg.util_avg / (cfs_rq->avg.load_avg + 1) * se->load.weight`
- This causes util_avg to be inflated by 1024x before capping, affecting load balancing decisions for newly created tasks
- Most evident when the calculated value would exceed the cap, leading to suboptimal initial utilization estimates

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create initial tasks to populate CFS averages, then observe inflated util_avg on new task creation:
- `setup()`: Create 2-3 initial tasks with `kstep_task_create()`, pin to CPU 1 with `kstep_task_pin()`
- `run()`: Wake initial tasks with `kstep_task_wakeup()`, run several ticks with `kstep_tick_repeat(50)` to build up cfs_rq averages
- Create new test task, examine its `se->avg.util_avg` immediately after creation/wakeup
- Log both the raw `se->load.weight` and `se_weight(se)` values to verify 1024x difference on 64-bit
- Compare expected util_avg (using se_weight) vs actual util_avg to detect 1024x inflation
- Use `on_tick_begin()` callback to monitor util_avg values and detect when capping occurs
- On buggy kernel: util_avg calculation uses inflated weight, on fixed kernel: uses correct weight
