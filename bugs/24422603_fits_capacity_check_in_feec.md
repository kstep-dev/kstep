# Fix fits_capacity() check in feec()

- **Commit:** 244226035a1f9b2b6c326e55ae5188fab4f428cb
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS, EAS (Energy-Aware Scheduling), uclamp

## Bug Description

When a task has `uclamp_min >= 0.8 * 1024` (819.2), the energy-aware CPU selection in `find_energy_efficient_cpu()` incorrectly rejects all CPUs as unsuitable. This causes the task to always be placed on the previous CPU rather than finding an optimal energy-efficient placement. The bug occurs because `fits_capacity()` with clamped utility values fails to properly account for uclamp constraints.

## Root Cause

The original code used `fits_capacity(util, cpu_cap)` directly with the result of `uclamp_rq_util_with()`, which clamps the utility value. However, `fits_capacity()` performs a simple comparison without considering that uclamp constraints may require the CPU to accept higher utilization. This causes valid placements to be incorrectly rejected when uclamp_min values are high.

## Fix Summary

The fix replaces `fits_capacity()` with the new `util_fits_cpu()` function that properly evaluates CPU capacity considering both the raw utility and uclamp constraints separately. It also introduces helper functions (`uclamp_rq_get()`, `uclamp_rq_set()`, `uclamp_rq_is_idle()`) for cleaner and safer access to rq-level uclamp values. The fix correctly applies max-aggregated uclamp values without clamping the utility, allowing `util_fits_cpu()` to make proper capacity decisions.

## Triggering Conditions

The bug triggers in Energy-Aware Scheduling (EAS) during `find_energy_efficient_cpu()` when:
- A task has `uclamp_min >= 0.8 * 1024` (≈819)
- The system is EAS-enabled with multiple performance domains and asymmetric CPU capacities
- Task wakeup path calls `feec()` for energy-efficient CPU selection
- The old `fits_capacity(uclamp_rq_util_with(rq, util, p), cpu_cap)` check rejects all CPUs
- Task placement defaults to previous CPU instead of optimal energy-efficient placement
- Race condition: rq uclamp state aggregated with task's high uclamp_min exceeds CPU capacity threshold

## Reproduce Strategy (kSTEP)

Setup asymmetric CPU topology with EAS domains using `kstep_cpu_set_capacity()` to create different capacity CPUs (e.g., CPUs 1-2 at full capacity, CPUs 3-4 at half capacity). Create a task with high uclamp_min ≥820 using cgroup controls via `kstep_cgroup_create()` and `kstep_cgroup_write()` to set `cpu.uclamp.min`. Use `kstep_task_pin()` to initially place the task on a low-capacity CPU, then call `kstep_task_wakeup()` to trigger EAS CPU selection. Monitor task placement using `on_tick_begin()` callback with `kstep_output_curr_task()` to verify the task remains on previous CPU instead of migrating to energy-optimal higher-capacity CPUs. Detection: log CPU placement decisions and confirm suboptimal energy choices when high uclamp_min tasks wake up.
