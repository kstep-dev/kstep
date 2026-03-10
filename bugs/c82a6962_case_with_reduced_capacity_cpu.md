# sched/fair: fix case with reduced capacity CPU

- **Commit:** c82a69629c53eda5233f13fc11c3c01585ef48a2
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

CPUs with reduced capacity (due to running other activities like IRQs or RT tasks) were being incorrectly classified as `group_fully_busy` instead of `group_misfit_task` during load balancing. This prevented the scheduler from moving CFS tasks to CPUs with more available capacity, resulting in tasks being forced to run on capacity-constrained CPUs even when better CPUs were available.

## Root Cause

The rework of the load balance logic added a check for misfit tasks, but it was gated behind the `SD_ASYM_CPUCAPACITY` flag and only checked `rq->misfit_task_load` (for tasks that don't fit the CPU architecture). Reduced capacity from runtime activities was not detected as a misfit condition unless explicitly marked by the architecture, leaving reduced capacity CPUs to be classified as fully busy when they had only one task running.

## Fix Summary

The fix adds a `sched_reduced_capacity()` function to detect CPUs running a single task with reduced available capacity. The load balancing code now classifies such CPUs as `group_misfit_task` regardless of the `SD_ASYM_CPUCAPACITY` flag, enabling task migration to less constrained CPUs. The migration path is also differentiated: `SD_ASYM_CPUCAPACITY` cases use `migrate_misfit`, while reduced capacity cases use `migrate_load`.

## Triggering Conditions

- CPU with reduced available capacity due to IRQs, RT tasks, or other non-CFS activities
- Exactly 1 CFS task running on the reduced-capacity CPU (h_nr_running == 1)
- Load balancing triggered during idle/newly_idle state (env->idle != CPU_NOT_IDLE)
- No SD_ASYM_CPUCAPACITY flag set in the scheduling domain
- Available CPUs with higher capacity where the task could be migrated
- `check_cpu_capacity()` returns true indicating capacity constraint
- Load balancing incorrectly classifies the CPU as `group_fully_busy` instead of `group_misfit_task`

## Reproduce Strategy (kSTEP)

- Setup: 3+ CPUs, set CPU 1 to reduced capacity using `kstep_cpu_set_capacity(1, SCHED_CAPACITY_SCALE/2)`
- Create single CFS task and pin to CPU 1 using `kstep_task_pin(task, 1, 1)` 
- Keep other CPUs (2+) at full capacity with spare availability
- Use `on_sched_balance_selected` callback to monitor load balance decisions
- Run `kstep_tick_repeat()` to trigger periodic load balancing
- Check via `kstep_output_balance()` if CPU 1 is classified as `group_fully_busy` (bug) vs `group_misfit_task` (fixed)
- Observe task migration patterns: bug prevents migration, fix enables migration to higher-capacity CPUs
- Validate using task pinning removal and checking if task moves from CPU 1 to CPUs 2+
- Log capacity values and group classifications to confirm misclassification occurs
