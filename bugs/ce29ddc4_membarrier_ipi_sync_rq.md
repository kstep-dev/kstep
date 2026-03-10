# sched/membarrier: fix missing local execution of ipi_sync_rq_state()

- **Commit:** ce29ddc47b91f97e7f69a0fb7cbb5845f52a9825
- **Affected file(s):** kernel/sched/membarrier.c
- **Subsystem:** core (membarrier)

## Bug Description

The function `sync_runqueues_membarrier_state()` is responsible for synchronizing membarrier state from an mm_struct to all runqueues currently executing tasks that use that mm. However, `smp_call_function_many()` deliberately skips the current runqueue, causing the current CPU's runqueue to not receive the membarrier state update when it should. This results in incorrect membarrier semantics: the current CPU may fail to issue required memory barriers, leading to potential memory visibility issues.

## Root Cause

`smp_call_function_many()` is designed to execute a function on all specified CPUs *except* the current CPU. The original code did not account for this semantic, assuming all CPUs in the mask would be updated. When the current CPU is running a task using the specified mm, its runqueue's membarrier state is left stale, causing memory barrier operations to be incorrectly omitted.

## Fix Summary

Replace `smp_call_function_many()` with `on_each_cpu_mask()`, which executes the function on all CPUs in the mask, *including* the current one. This ensures every runqueue with a relevant task receives the necessary membarrier state update.

## Triggering Conditions

The bug is triggered when `sync_runqueues_membarrier_state()` is called while the current CPU is running a task that shares the same `mm_struct` as the target memory space. The function builds a CPU mask (`tmpmask`) containing all CPUs running tasks with the matching mm, including the current CPU. However, `smp_call_function_many()` deliberately excludes the current CPU from IPI delivery, leaving the current runqueue's membarrier state unsynced. This requires:
- Multi-CPU system (need at least CPU 1-2, since CPU 0 is driver-reserved)
- Current CPU running a task with the target mm when sync is triggered
- Membarrier registration/operation that calls `sync_runqueues_membarrier_state()`
- Multiple threads/processes sharing the same mm across different CPUs

## Reproduce Strategy (kSTEP)

Create 2+ tasks sharing memory space and trigger membarrier sync while current CPU runs one of them:
- **CPUs needed**: At least 2 (use CPUs 1-2, since CPU 0 reserved for driver)
- **Setup**: Create tasks using `kstep_task_create()` and pin them to different CPUs with `kstep_task_pin(task1, 1, 1)` and `kstep_task_pin(task2, 2, 2)`
- **Sequence**: Use `kstep_task_fork(parent, 1)` to create child sharing mm, then `kstep_task_wakeup()` both parent and child
- **Trigger**: Call membarrier syscall (via `kstep_write("/proc/sys/kernel/sched_membarrier", "1", 1)`) or use direct kernel calls if available in kSTEP
- **Detection**: Monitor runqueue membarrier state via `on_tick_end()` callback, checking if current CPU's rq state matches expected value
- **Observation**: Log current CPU's runqueue state before/after sync to detect missing update
