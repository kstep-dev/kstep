# Revert "sched_ext: Skip per-CPU tasks in scx_bpf_reenqueue_local()"

- **Commit:** 0b47b6c3543efd65f2e620e359b05f4938314fbd
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (extensible scheduler class)

## Bug Description

When a higher scheduling class (e.g., SCHED_FIFO) preempts a CPU, the running task is inserted into the local DSQ as migration-disabled. A previous commit attempted to optimize `scx_bpf_reenqueue_local()` by skipping such migration-disabled tasks. However, this optimization causes tasks to remain confined to heavily contended CPUs, resulting in severe CPU underutilization. For example, a SCHED_EXT task pinned to CPU0 (contended by a SCHED_FIFO task consuming 99% CPU) would achieve only ~1% utilization instead of migrating to an idle CPU1 and achieving ~100% utilization.

## Root Cause

The previous commit incorrectly assumed that migration-disabled tasks could not be migrated and thus added conditions to skip them in `scx_bpf_reenqueue_local()`. However, when such tasks are re-enqueued via `do_enqueue_task()`, the scheduler's BPF `ops.enqueue()` callback can reassess their placement and potentially migrate them to other available CPUs. By skipping re-enqueueing, the scheduler loses the opportunity to relocate tasks away from overcontended CPUs.

## Fix Summary

This commit removes the conditions that skip migration-disabled tasks (`is_migration_disabled(p)`) and per-CPU tasks (`p->nr_cpus_allowed == 1`) from `scx_bpf_reenqueue_local()`, retaining only the skip for tasks pending migration. This allows the scheduler to re-enqueue these tasks with the `SCX_ENQ_REENQ` flag, enabling the BPF scheduler to make better placement decisions and migrate tasks to less contended CPUs when appropriate.

## Triggering Conditions

This bug occurs when a SCHED_EXT task becomes migration-disabled after being preempted by a higher scheduling class task on a contended CPU. Specifically: (1) A SCHED_EXT task initially runs on a CPU that becomes heavily contended by a higher priority task (SCHED_FIFO/RT), (2) When preempted, the SCHED_EXT task is inserted into the local DSQ as migration-disabled, (3) `scx_bpf_reenqueue_local()` is called from `ops.cpu_release()` but incorrectly skips the migration-disabled task, (4) The task remains confined to the contended CPU achieving minimal utilization (~1%) instead of migrating to idle CPUs. The bug requires multiple CPUs where at least one idle CPU is available for migration, and a CPU topology that allows the SCHED_EXT task to run on multiple CPUs initially.

## Reproduce Strategy (kSTEP)

Set up a 3-CPU system (CPU0 for driver, CPU1-2 for workload) with `kstep_topo_init()` and basic topology. Create a SCHED_EXT task using `kstep_task_create()` and allow it to run on CPU1-2 with `kstep_task_pin(task_ext, 1, 2)`. Create a SCHED_FIFO task with `kstep_task_fifo(task_rt)` and pin it exclusively to CPU1 with `kstep_task_pin(task_rt, 1, 1)`. In `run()`, wake both tasks with `kstep_task_wakeup()` and run several ticks with `kstep_tick_repeat()`. Use `on_tick_begin` callback to log CPU utilization via `kstep_output_nr_running()` and track which CPU each task runs on. The bug manifests when the SCHED_EXT task remains on CPU1 (low utilization due to RT contention) instead of migrating to idle CPU2. Monitor task placement and CPU utilization over 100+ ticks to confirm the SCHED_EXT task stays stuck on the contended CPU1.
