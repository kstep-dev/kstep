# nohz/full, sched/rt: Fix missed tick-reenabling bug in dequeue_task_rt()

- **Commit:** 5c66d1b9b30f737fcef85a0b75bfe0590e16b62a
- **Affected file(s):** kernel/sched/rt.c
- **Subsystem:** RT (Real-Time scheduler)

## Bug Description

On nohz_full systems, the tick dependency check in `sched_update_tick_dependency()` is called BEFORE the RT task counter is decremented. This prevents the tick from being re-enabled even when all SCHED_RT tasks have been dequeued and only SCHED_OTHER tasks remain. The tick remains unnecessarily disabled, impacting performance and defeating the purpose of the nohz_full mechanism.

## Root Cause

In `dequeue_rt_stack()`, the call to `dequeue_top_rt_rq()` (which calls `sub_nr_running()` and triggers `sched_update_tick_dependency()`) happens BEFORE `__dequeue_rt_entity()` calls `dec_rt_tasks()` to decrement `rt_rq->rt_nr_running`. The tick dependency check examines the outdated `rt_nr_running` value, causing it to misidentify the system state and fail to re-enable the tick.

## Fix Summary

The fix reorders the dequeue operations by capturing `rt_nr_running` before decrementing it via `__dequeue_rt_entity()`, then passing the captured value to `dequeue_top_rt_rq()` which is called after. This ensures the tick dependency check uses the correct pre-decrement value while the actual counters are updated in the correct order, matching the behavior of other scheduler classes.

## Triggering Conditions

This bug triggers on nohz_full systems when the last RT task on a CPU is dequeued while SCHED_OTHER tasks remain runnable. The specific sequence requires:
- A CPU running with nohz_full (CONFIG_NO_HZ_FULL=y) where the tick is currently disabled due to RT tasks
- At least one SCHED_RT task running on that CPU (causing the tick to be disabled)
- Multiple SCHED_OTHER tasks also runnable on the same CPU
- The RT task being dequeued (via sleep, migration, or termination), leaving only SCHED_OTHER tasks
- The bug occurs in `dequeue_rt_stack()` where `sched_update_tick_dependency()` checks `rt_nr_running` before it's decremented

## Reproduce Strategy (kSTEP)

Reproduce the tick dependency bug using 2+ CPUs (CPU 0 reserved for driver):
- Use `kstep_sysctl_write()` to enable nohz_full features on CPU 1
- Create one RT task with `kstep_task_create()` + `kstep_task_fifo()`, pin to CPU 1 with `kstep_task_pin()`
- Create multiple CFS tasks with `kstep_task_create()`, pin to CPU 1 to establish runnable SCHED_OTHER load
- Run ticks with `kstep_tick_repeat()` to let RT task run and tick get disabled due to RT presence
- Use the `on_tick_begin()` callback to monitor tick state and log CPU 1's `rt_rq->rt_nr_running` and `rq->nr_running`
- Pause the RT task with `kstep_task_pause()` to trigger dequeue sequence
- Check if tick dependency logic incorrectly sees stale `rt_nr_running` (should re-enable tick but doesn't)
- The bug manifests when tick remains disabled despite only SCHED_OTHER tasks remaining runnable
