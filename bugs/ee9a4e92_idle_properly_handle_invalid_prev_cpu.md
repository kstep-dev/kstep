# sched_ext: idle: Properly handle invalid prev_cpu during idle selection

- **Commit:** ee9a4e92799d72b1a2237d76065562b1f1cf334f
- **Affected file(s):** kernel/sched/ext_idle.c
- **Subsystem:** sched_ext (idle CPU selection)

## Bug Description

The default idle selection policy does not properly validate that @prev_cpu is within the task's allowed CPUs before using it to make scheduling decisions. This can result in returning an idle CPU that the task is not allowed to run on, violating the assumption that the returned CPU must be in the task's allowed cpumask. This breaks scheduler correctness and can cause scheduling inefficiencies or stalls in certain cases.

## Root Cause

The function previously checked if prev_cpu was in the allowed mask in only one specific code path (when handling explicit cpus_allowed via cpumask_and), but failed to validate it in other critical paths where prev_cpu was being used—specifically the WAKE_SYNC optimization, SMT selection, and final prev_cpu fallback. This inconsistency allowed invalid CPUs to be selected.

## Fix Summary

The fix introduces an early validation check that determines whether prev_cpu is in the allowed cpumask and stores the result in is_prev_allowed. This flag is then consistently checked before using prev_cpu in all code paths (WAKE_SYNC, SMT, and fallback), ensuring the returned CPU is always within the task's allowed cpumask while preserving locality-aware selection.

## Triggering Conditions

The bug is triggered in the sched_ext idle CPU selection path (`scx_select_cpu_dfl`) when `prev_cpu` is not within the task's allowed cpumask. This occurs when: (1) Task affinity is changed after the task was last scheduled but before idle selection runs, especially during BPF test_run contexts; (2) A BPF scheduler provides an invalid `prev_cpu` as a placement hint that's outside the allowed mask; (3) The task's `cpus_ptr` has been updated to exclude the previous CPU. The bug manifests when WAKE_SYNC optimization, SMT sibling selection, or final `prev_cpu` fallback attempts to use the invalid CPU without validation, potentially returning an unusable CPU for scheduling and causing inefficiencies or task stalls.

## Reproduce Strategy (kSTEP)

Use 3+ CPUs (CPU 0 reserved for driver). Create a task with initial affinity to CPUs 1-2, run it on CPU 1, then change affinity to exclude CPU 1 while the task is sleeping. Trigger a wakeup that would normally prefer the previous CPU. In `setup()`: create task with `kstep_task_create()`, set affinity with `kstep_cgroup_create()` and `kstep_cgroup_set_cpuset()`. In `run()`: wake task with `kstep_task_wakeup()`, run with `kstep_tick_repeat()` to establish prev_cpu, pause with `kstep_task_pause()`, change affinity to exclude prev_cpu using `kstep_cgroup_set_cpuset()`, then wake again. Use `on_tick_begin()` callback to log CPU selections and detect when an invalid CPU is chosen. Check that the selected CPU is within the new allowed mask to detect the bug.
