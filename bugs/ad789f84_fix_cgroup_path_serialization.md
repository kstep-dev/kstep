# sched/debug: Fix cgroup_path[] serialization

- **Commit:** ad789f84c9a145f8a18744c0387cec22ec51651e
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** Core scheduler debug

## Bug Description

The scheduler debug code holds `sched_debug_lock` with interrupts disabled for the entire duration of printing CPU scheduling statistics. On systems with many CPUs and under concurrent sysrq-t (or similar debug) operations, this lock contention combined with disabled interrupts can trigger a hard lockup panic. The lock is only needed to protect access to the global `cgroup_path[]` buffer, but the original implementation serializes the entire printing operation.

## Root Cause

The `print_cpu()` function acquired `sched_debug_lock` with `spin_lock_irqsave()` at the start and held it with interrupts disabled throughout multiple calls to `print_cfs_stats()`, `print_rt_stats()`, `print_dl_stats()`, and `print_rq()`. This unnecessarily long critical section with interrupts disabled can cause hard lockups if multiple CPUs contend for the lock during concurrent debug operations. The lock's actual purpose is only to protect concurrent access to the shared `group_path[]` buffer, not the entire printing operation.

## Fix Summary

The fix decouples lock protection from the entire printing operation. It replaces the global lock with a new `SEQ_printf_task_group_path()` macro that uses non-blocking `spin_trylock()` to protect only the critical section accessing `group_path[]`. If the lock cannot be acquired, other callers use a smaller stack buffer (128 bytes) instead, with a "..." suffix to indicate possible truncation. This eliminates the long critical section with interrupts disabled while still protecting the shared buffer.

## Triggering Conditions

The bug occurs when multiple CPUs simultaneously attempt to access scheduler debug information through `/proc/sched_debug` or sysrq-t operations. The `print_cpu()` function in `kernel/sched/debug.c` holds `sched_debug_lock` with interrupts disabled during the entire printing sequence (`print_cfs_stats`, `print_rt_stats`, `print_dl_stats`, `print_rq`). On systems with many CPUs, when concurrent debug operations create lock contention, CPUs waiting for the lock with interrupts disabled can trigger hard lockup detection. The critical factor is the combination of: (1) multiple concurrent callers to scheduler debug functions, (2) long-held spinlock with interrupts disabled, and (3) slow console output (especially serial consoles) extending the critical section duration.

## Reproduce Strategy (kSTEP)

Reproducing this bug requires creating lock contention on the scheduler debug path. Use at least 3 CPUs (excluding CPU 0). In `setup()`, create multiple task groups with `kstep_cgroup_create()` to generate complex cgroup hierarchies that will produce lengthy debug output. Create several tasks with `kstep_task_create()` and assign them to different cgroups using `kstep_cgroup_add_task()`. In `run()`, use `kstep_tick_repeat()` to establish scheduler state, then trigger concurrent debug operations by directly calling `kstep_print_sched_debug()` from multiple contexts or use kthreads with `kstep_kthread_create()` to simulate parallel sysrq-t calls. Monitor for lockup conditions using `on_tick_begin()` callbacks to detect when scheduler ticks stop advancing. The bug manifests as hard lockup panics when debug printing takes too long with interrupts disabled, so look for system hangs or lockup detector warnings in the output logs.
