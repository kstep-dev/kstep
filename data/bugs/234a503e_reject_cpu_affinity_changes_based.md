# sched: Reject CPU affinity changes based on task_cpu_possible_mask()

- **Commit:** 234a503e670be01f72841be9fcf68dfb89a1fa8b
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The scheduler's `__set_cpus_allowed_ptr()` function did not validate that the requested CPU affinity mask for a task was a subset of the CPUs the task can actually execute on (its task_cpu_possible_mask). This allowed user code or kernel code to set a task's CPU affinity to include CPUs it cannot execute on, violating the scheduling constraint that a task's cpus_mask should only contain CPUs capable of running it (except when forced).

## Root Cause

There was no validation check in `__set_cpus_allowed_ptr()` to ensure that the requested affinity mask (`new_mask`) was a subset of the CPUs returned by `task_cpu_possible_mask()`. This constraint was not enforced when setting CPU affinity for regular (non-kernel) threads, allowing invalid states where a task could have CPUs in its affinity mask that it cannot execute on.

## Fix Summary

The fix adds an explicit validation check that rejects CPU affinity change requests for non-kernel threads if the requested mask is not a subset of `task_cpu_possible_mask()`. When such an invalid request is detected, the function returns `-EINVAL` and rejects the change, ensuring that a task's cpus_mask never contains CPUs incapable of executing it (except in forced cases).

## Triggering Conditions

The bug requires setting CPU affinity for a non-kernel task where the requested mask includes CPUs outside the task's `task_cpu_possible_mask()`. This typically involves:
- A heterogeneous CPU topology where some CPUs have different capabilities (e.g., big.LITTLE ARM systems)
- Task creation on CPUs with restricted execution capabilities
- Explicit calls to `sched_setaffinity()` or related syscalls with invalid CPU masks
- Race conditions during CPU hotplug where task affinity is updated while CPU capabilities change
- The target task must not be a kernel thread (`PF_KTHREAD` flag cleared)

## Reproduce Strategy (kSTEP)

Set up asymmetric CPU topology with different capabilities and attempt invalid affinity changes:
- Use 4+ CPUs: CPU 0 (driver), CPUs 1-2 (full capability), CPUs 3-4 (restricted capability)
- Call `kstep_cpu_set_capacity()` to create heterogeneous topology (e.g., CPU 3-4 at 50% capacity)
- Create user tasks with `kstep_task_create()` and pin them initially to restricted CPUs
- Use direct syscall emulation or task manipulation to attempt setting affinity to include incompatible CPUs
- Monitor return values from affinity change operations - pre-fix should succeed, post-fix should return -EINVAL
- Log `task_cpu_possible_mask()` contents vs requested affinity mask to verify mismatch
- Verify task execution behavior: pre-fix allows invalid scheduling, post-fix maintains constraints
