# sched: Fix unreliable rseq cpu_id for new tasks

- **Commit:** ce3614daabea8a2d01c1dd17ae41d1ec5e5ae7db
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The `__rseq_abi.cpu_id` field contains incorrect values for newly created tasks right after they issue `sched_setaffinity()`. This causes glibc's rseq-based tests to fail with "Unexpected CPU" errors. The inaccurate CPU ID can corrupt per-cpu data accessed within rseq critical sections in user-space, leading to data corruption.

## Root Cause

The scheduler calls `__set_task_cpu()` directly in `sched_fork()` and `wake_up_new_task()` to set the CPU for new tasks. This direct call bypasses the `rseq_migrate()` function, which is normally invoked by the standard `set_task_cpu()` wrapper. Without this call, the rseq per-CPU state is not updated to reflect the actual CPU assignment, causing `__rseq_abi.cpu_id` to become stale.

## Fix Summary

The fix adds explicit `rseq_migrate(p)` calls immediately before `__set_task_cpu()` in both `sched_fork()` and `wake_up_new_task()` functions. This ensures that the rseq per-CPU state is correctly synchronized when new tasks are assigned to a CPU, making `__rseq_abi.cpu_id` reliable for use in rseq critical sections.
