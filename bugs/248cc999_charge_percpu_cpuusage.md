# sched/cpuacct: Fix charge percpu cpuusage

- **Commit:** 248cc9993d1cc12b8e9ed716cc3fc09f6c3517dd
- **Affected file(s):** kernel/sched/cpuacct.c
- **Subsystem:** core

## Bug Description

The `cpuacct_charge()` function uses `__this_cpu_add()` to charge CPU time to a task's cpuacct group. However, this function can be called from `update_curr()` during load balancing on any CPU, not just the CPU where the task is actually running. When `__this_cpu_add()` is used from a different CPU than the task's CPU, the CPU time gets charged to the wrong CPU's per-cpu accounting, causing incorrect cpuacct measurements.

## Root Cause

The original code assumes `cpuacct_charge()` is always called on the CPU where the task is running. However, during load balancing in `load_balance()`, `update_curr()` can be invoked on a different CPU than the one executing the task. Using `__this_cpu_add()` blindly adds the cputime to the current CPU's (the one executing the code) accounting instead of the task's actual CPU. This is a per-cpu data access mismatch that violates the invariant that cpuacct must track per-task CPU usage on the correct CPU.

## Fix Summary

The fix explicitly retrieves the task's CPU using `task_cpu(tsk)` at the start of `cpuacct_charge()` and uses `per_cpu_ptr(ca->cpuusage, cpu)` to access the correct per-cpu data structure. This ensures the CPU time is always charged to the correct CPU regardless of which CPU the function is called from.

## Triggering Conditions

The bug requires load balancing to occur where `update_curr()` is called on a CPU different from where the task is running. This happens when:
- Multiple CPUs with uneven load distribution trigger `load_balance()`
- During load balancing, `update_curr()` gets invoked on the balancing CPU (not task's CPU)
- The task must be part of a cpuacct cgroup to observe incorrect per-CPU usage accounting
- Tasks are actively running and accumulating CPU time during the load balancing window
- The timing window where `cpuacct_charge()` uses `__this_cpu_add()` from the wrong CPU context

## Reproduce Strategy (kSTEP)

Use 3+ CPUs (CPU 0 reserved) to create load imbalance and trigger load balancing:
- In `setup()`: Create cpuacct cgroup with `kstep_cgroup_create()` and configure multiple tasks with `kstep_task_create()`
- Pin tasks unevenly across CPUs using `kstep_task_pin()` to force load imbalance (e.g., 2 tasks on CPU 1, none on CPU 2)
- In `run()`: Use `kstep_task_wakeup()` and `kstep_tick_repeat()` to accumulate runtime and trigger periodic load balancing
- Monitor with `on_sched_balance_begin()` callback to detect when load balancing occurs
- Track per-CPU cpuacct usage via cgroup files and compare with expected task CPU assignments
- Bug detected when CPU time appears on wrong CPU's accounting during/after load balancing events
