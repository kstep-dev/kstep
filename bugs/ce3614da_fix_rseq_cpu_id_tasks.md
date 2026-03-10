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

## Triggering Conditions

The bug occurs when new tasks are created and assigned to CPUs through the scheduler's core fork/wakeup paths:
- Tasks must be newly forked via `sched_fork()` or woken via `wake_up_new_task()`
- The scheduler must call `__set_task_cpu()` directly to assign the task to a CPU
- The task's rseq state becomes stale because `rseq_migrate()` is bypassed
- User-space code using rseq critical sections will see incorrect `__rseq_abi.cpu_id` values
- Most easily triggered by processes issuing `sched_setaffinity()` after fork
- Race condition where CPU assignment occurs before rseq state synchronization
- Affects any kernel version with rseq support (v4.18+) that has user-space rseq consumers

## Reproduce Strategy (kSTEP)

This bug requires user-space rseq state tracking which is difficult to reproduce in kernel-only kSTEP:
- Use 2 CPUs minimum (CPU 0 reserved for driver, CPU 1+ for tasks)
- In `setup()`: Create multiple tasks via `kstep_task_create()` to simulate fork scenarios
- In `run()`: Use `kstep_task_pin()` to force CPU affinity changes that trigger the buggy path
- Call `kstep_task_wakeup()` repeatedly to exercise `wake_up_new_task()` code path  
- Use `kstep_task_fork()` to create child processes that go through `sched_fork()`
- Monitor task CPU assignments and detect inconsistencies via `on_tick_begin()` callback
- Check if task->cpu differs from expected CPU after affinity operations
- Log task state transitions and CPU assignments to detect rseq state staleness
- The bug manifests as tasks reporting incorrect CPU IDs after CPU reassignment
