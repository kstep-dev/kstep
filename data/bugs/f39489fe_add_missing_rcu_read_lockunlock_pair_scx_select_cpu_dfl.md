# sched_ext: add a missing rcu_read_lock/unlock pair at scx_select_cpu_dfl()

- **Commit:** f39489fea677ad78ca4ce1ab2d204a6639b868dc
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext scheduler)

## Bug Description

The `scx_select_cpu_dfl()` function dereferences a pointer to the `sched_domain` structure without holding an RCU read lock, which triggers RCU lockdep warnings. This unprotected dereference can cause invalid memory access in the worst case, as the data structure may be freed while being accessed. The bug manifests as a "suspicious rcu_dereference_check() usage" warning when the default CPU selection policy attempts to retrieve the LLC CPU mask.

## Root Cause

The function retrieves the LLC CPU mask by dereferencing a `sched_domain` pointer, which is RCU-protected data. However, the code was not wrapped in an `rcu_read_lock()/rcu_read_unlock()` pair, violating RCU synchronization requirements. When the kernel's RCU lockdep checker inspects the dereference, it detects that the appropriate RCU read-side critical section is missing, triggering the warning.

## Fix Summary

The fix adds `rcu_read_lock()` at the beginning of the function to initiate an RCU read-side critical section, and adds corresponding `rcu_read_unlock()` calls at both return paths to properly exit the critical section. This protects all accesses to the RCU-protected `sched_domain` pointer and satisfies RCU synchronization requirements.

## Triggering Conditions

The bug occurs in the sched_ext subsystem's default CPU selection path when `scx_select_cpu_dfl()` is called during task wakeup or migration. The function accesses the LLC (Last Level Cache) CPU mask from a `sched_domain` structure at line 3323 (in `kernel/sched/ext.c`) without proper RCU protection. The triggering conditions include:
- sched_ext scheduler must be active with the default CPU selection policy
- A task wakeup/migration event triggers `select_task_rq_scx()` → `scx_select_cpu_dfl()`
- The code path reaches the LLC CPU mask dereference (accessing scheduler domain topology)
- RCU lockdep checking must be enabled (CONFIG_PROVE_RCU=y) to detect the violation
- The bug manifests as an RCU lockdep warning and potential memory corruption if RCU grace period frees the sched_domain during access

## Reproduce Strategy (kSTEP)

To reproduce this bug, create a multi-CPU topology and trigger task wakeups under sched_ext. Use at least 2 CPUs (CPU 1+ available for tasks). In `setup()`: configure LLC topology using `kstep_topo_init()`, `kstep_topo_set_cls()` with clustered CPUs, and `kstep_topo_apply()`. Create multiple tasks with `kstep_task_create()` and use `kstep_task_pin()` to distribute them across CPUs. In `run()`: repeatedly call `kstep_task_pause()` and `kstep_task_wakeup()` on different CPUs to trigger the `scx_select_cpu_dfl()` path during task migration. Use the `on_tick_begin` callback to log task states and detect the RCU warning. The bug should manifest as "suspicious rcu_dereference_check() usage" warnings in kernel logs when tasks migrate and the scheduler accesses LLC topology information without RCU protection.
