# sched/deadline: Fix potential race in dl_add_task_root_domain()

- **Commit:** 64e6fa76610ec970cfa8296ed057907a4b384ca5
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline scheduler

## Bug Description

A race condition exists in `dl_add_task_root_domain()` when accessing the per-CPU variable `local_cpu_mask_dl`. Without preemption disabled, a thread can get a pointer to the per-CPU variable on one CPU, then be preempted and migrated to another CPU, continuing to use the original CPU's variable. Meanwhile, the scheduler on the original CPU may also access the same per-CPU buffer, causing concurrent corruption of the shared buffer.

## Root Cause

The function violates the access rule for `local_cpu_mask_dl`, which requires being called on the local CPU with preemption disabled. The original code accessed `this_cpu_cpumask_var_ptr(local_cpu_mask_dl)` before acquiring locks, allowing a thread to be preempted and migrate to a different CPU while holding a pointer to the original CPU's per-CPU variable. This creates a window for concurrent access from multiple contexts using the same per-CPU buffer.

## Fix Summary

The fix moves the `this_cpu_cpumask_var_ptr(local_cpu_mask_dl)` access to after the early return check but while holding the `p->pi_lock` spinlock (via `raw_spin_lock_irqsave`), ensuring the pointer is obtained in a protected context that prevents problematic preemption and migration during the critical usage window.

## Triggering Conditions

The bug requires a deadline task undergoing root domain addition via `dl_add_task_root_domain()` during cpuset changes. Specifically:
- A deadline task exists that needs root domain reconfiguration (e.g., during CPU hotplug or cpuset partition changes)
- The task calling `dl_add_task_root_domain()` must be preemptible and migratable
- High scheduling pressure or specific timing where the task gets preempted and migrated between getting the per-CPU pointer and using it
- Concurrent deadline scheduler activity (e.g., `find_later_rq()`) on the original CPU that also accesses `local_cpu_mask_dl`
- Multiple CPUs with active deadline scheduling to create migration opportunities

## Reproduce Strategy (kSTEP)

Create a multi-CPU system (2+ CPUs beyond CPU 0) and simulate heavy cpuset manipulation with deadline tasks:
- Setup: Use `kstep_topo_init()` and `kstep_topo_apply()` to create 4+ CPUs, create multiple deadline tasks with `kstep_task_create()`
- Run: Create deadline tasks on different CPUs, trigger cpuset changes via `kstep_cgroup_create()` and `kstep_cgroup_set_cpuset()` while moving tasks between cgroups with `kstep_cgroup_add_task()`
- Concurrently use `kstep_task_wakeup()` and `kstep_tick_repeat()` to maintain high scheduling activity
- Use `on_tick_begin` callback to monitor per-CPU `local_cpu_mask_dl` state and detect corruption patterns
- Check for memory corruption in `local_cpu_mask_dl` by logging its contents across different CPUs during cpuset operations
- The bug manifests as inconsistent or corrupted cpumask data when the same per-CPU buffer is accessed concurrently
