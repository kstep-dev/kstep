# sched: Fix CPU hotplug / tighten is_per_cpu_kthread()

- **Commit:** 5ba2ffba13a1e24e7b153683e97300f9cc6f605a
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (CPU hotplug, task migration)

## Bug Description

CPU hotplug fails to properly migrate regular kernel threads during CPU offline because the scheduler incorrectly classifies any kernel thread with a single CPU affinity as a per-CPU kthread. When a CPU goes offline, per-CPU kthreads (like idle and hotplug threads) must stay, but regular kthreads with single-CPU affinity should be migrated away. The broken behavior leaves these regular kthreads stuck on the dying CPU, causing hotplug failures.

## Root Cause

The previous commit (1cf12e08bc4d) consolidated task migration to handle migrate_disable() correctly but removed affinity breaking for all tasks classified as per-CPU kthreads. The `is_per_cpu_kthread()` function is too broad—it returns true for any kthread with single CPU affinity, conflating actual per-CPU kthreads (those with the KTHREAD_IS_PER_CPU flag) with regular kthreads that merely happen to have restricted affinity. This distinction is critical during CPU hotplug, where true per-CPU kthreads are required to remain (idle, hotplug, stop threads) but regular kthreads must be migrated.

## Fix Summary

The fix introduces proper differentiation by using `kthread_is_per_cpu()` infrastructure to identify true per-CPU kthreads and refines the CPU allowance logic in `is_cpu_allowed()` and `balance_push()`. It explicitly checks for KTHREAD_IS_PER_CPU flag and uses `balance_push` state to ensure regular kthreads are pushed away during hotplug while preserving required per-CPU threads.

## Triggering Conditions

The bug is triggered during CPU hotplug when a regular kernel thread (without KTHREAD_IS_PER_CPU flag) has single-CPU affinity and the target CPU goes offline. The scheduler's `is_cpu_allowed()` function incorrectly allows these threads to stay on the dying CPU because the old `is_per_cpu_kthread()` returns true for any kthread with single CPU affinity. The bug manifests when `balance_push` is active (CPU going offline) and there are regular kthreads pinned to that CPU that should be migrated but aren't. The race occurs between CPU deactivation and the balance_push mechanism failing to identify which kthreads are truly per-CPU versus regular kthreads with restricted affinity.

## Reproduce Strategy (kSTEP)

The reproduction requires simulating CPU hotplug with a regular kthread pinned to the target CPU. Use at least 2 CPUs (CPU 0 reserved for driver, test on CPU 1). In `setup()`, create a regular kthread using `kstep_kthread_create()` and pin it to CPU 1 with `kstep_task_pin()`. In `run()`, simulate CPU hotplug by manually setting `cpu_rq(1)->balance_push = true` to activate the balance_push mechanism. Call `kstep_tick_repeat()` to let the scheduler attempt task migration. Use `on_tick_end` callback to monitor task locations via `task->cpu` and check if the regular kthread remains stuck on CPU 1 despite balance_push being active. The bug is detected when the kthread fails to migrate away from the balance_push-enabled CPU, indicating the scheduler incorrectly classified it as a per-CPU kthread that should remain.
