# Fix per-CPU kthread and wakee stacking for asym CPU capacity

- **Commit:** 014ba44e8184e1acf93e0cbb7089ee847802f8f0
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair scheduling)

## Bug Description

For asymmetric CPU capacity systems, `select_idle_sibling()` has a fast-path that places tasks on the previous CPU when a per-CPU kthread wakes them up. However, this path did not validate that the target CPU has sufficient capacity to handle the task's current utilization. Since `uclamp.min` can change between task activations, a task may require significantly more capacity than it did on its last activation, causing it to be placed on an undersized CPU.

## Root Cause

The per-CPU kthread stacking code path (lines around 6400 in fair.c) only checked if the CPU was idle or had low runqueue load, but skipped the `asym_fits_capacity()` fitness check that is applied elsewhere in the function. This check is essential on asymmetric systems because `uclamp.min` constraints, which can be set independently per activation, might require the task to run on a higher-capacity CPU despite the CPU being available.

## Fix Summary

The fix adds an `asym_fits_capacity(task_util, prev)` check to the per-CPU kthread stacking condition, ensuring that CPU capacity constraints are validated even in this fast-path. This prevents tasks with elevated capacity requirements (due to uclamp.min) from being incorrectly placed on under-capacity CPUs.

## Triggering Conditions

This bug occurs in `select_idle_sibling()` when all of the following conditions hold:
- Running on an asymmetric CPU capacity system (e.g., big.LITTLE architecture)
- Current task is a per-CPU kthread (`is_per_cpu_kthread(current) == true`)
- Kthread is waking up a task whose previous CPU matches the current CPU (`prev == smp_processor_id()`)
- The current CPU's runqueue has at most 1 task (`this_rq()->nr_running <= 1`)
- The wakee task has uclamp.min set to a value requiring higher CPU capacity than the current CPU provides
- The current CPU lacks sufficient capacity to handle the task's utilization requirements

The race occurs when uclamp.min changes between task activations, causing the capacity fitness check to be bypassed in the kthread stacking fast-path.

## Reproduce Strategy (kSTEP)

Setup an asymmetric topology with 4+ CPUs where CPUs 1-2 are low-capacity and CPUs 3-4 are high-capacity:
- Use `kstep_cpu_set_capacity()` to set CPU 1-2 capacity to 512, CPU 3-4 to 1024
- Create a per-CPU kthread on CPU 1 using `kstep_kthread_create()` and `kstep_kthread_bind()`
- Create a regular task and place it on CPU 1 initially using `kstep_task_pin()`
- Set task's uclamp.min to require high capacity using cgroup controls via `kstep_cgroup_set_weight()`

In the run phase:
- Start the kthread on CPU 1 and have it wake the task via `kstep_kthread_syncwake()`
- Use `on_tick_begin()` callback to monitor task placement and log CPU assignments
- Detect the bug by checking if the high-capacity task was incorrectly placed on low-capacity CPU 1
- Verify correct behavior shows the task being moved to CPU 3 or 4 instead
