# sched: Fix hotplug vs CPU bandwidth control

- **Commit:** 120455c514f7321981c907a01c543b05aff3f254
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/rt.c
- **Subsystem:** core, RT, Deadline

## Bug Description

When a CPU is being deactivated/taken offline, tasks can be gained from bandwidth unthrottle operations after the system expects all tasks to have been migrated away. This occurs because `set_rq_offline()` was called too late in the shutdown sequence (during `sched_cpu_dying()`), after migration but when bandwidth operations could still add tasks back. Additionally, the RT and Deadline balancers were not checking the `rq->online` state, allowing them to attempt pulling tasks from CPUs that should be inactive.

## Root Cause

The scheduler's hotplug sequence was not properly coordinated: tasks were migrated away before CPU DYING, but `set_rq_offline()` was only called during DYING, not during DEACTIVATE when `cpu_active()` is cleared. This mismatch meant the balancers could still operate on deactivating CPUs via unthrottle mechanisms. The RT and Deadline pull checks did not verify `rq->online` status, relying only on priority/deadline conditions.

## Fix Summary

The fix moves `set_rq_offline()` from `sched_cpu_dying()` to `sched_cpu_deactivate()` to ensure the runqueue is marked offline when the CPU is deactivated, matching the semantics of when `cpu_active()` is cleared. Additionally, `need_pull_rt_task()` and `need_pull_dl_task()` now check `rq->online` before attempting to pull tasks, preventing balancing operations on offline CPUs.

## Triggering Conditions

This bug occurs during CPU hotplug when a CPU is being deactivated but not yet in DYING state. The race window exists between `set_cpu_active(cpu, false)` and the previous location of `set_rq_offline()` in `sched_cpu_dying()`. During this window, bandwidth unthrottle operations for RT/Deadline tasks can still occur, potentially adding tasks back to the runqueue after migration is expected to be complete. The RT and Deadline balancers (`need_pull_rt_task()` and `need_pull_dl_task()`) also ignore the CPU's active state and only check priority/deadline conditions, allowing them to pull tasks from deactivating CPUs. This requires a multi-CPU system with RT or Deadline tasks under bandwidth control experiencing throttling/unthrottling during CPU deactivation.

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved, target CPU for deactivation, and migration destination). Use `kstep_task_create()` to create RT tasks with `kstep_task_fifo()` scheduling policy. Set up bandwidth control with `kstep_cgroup_create()` and `kstep_cgroup_write()` to configure RT bandwidth limits that cause throttling. Pin tasks to the target CPU with `kstep_task_pin()`. In `run()`, trigger bandwidth throttling by running tasks beyond limits using `kstep_tick_repeat()`, then simulate CPU deactivation timing by manually clearing `cpu_active()` for the target CPU while triggering unthrottle operations. Use `on_tick_begin()` callback to monitor task migration and `on_sched_balance_selected()` to observe RT/DL balancing attempts on the deactivating CPU. Check for tasks reappearing on the target CPU runqueue after migration is expected complete to detect the bug.
