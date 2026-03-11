# sched/fair: Fix pelt clock sync when entering idle

- **Commit:** 98c88dc8a1ace642d9021b103b28cba7b51e3abc
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/idle.c
- **Subsystem:** CFS (fair scheduling), PELT clock

## Bug Description

After commit 17e3e88ed0b6, a regression in util_avg tracking of RT run queues was observed. The issue occurs when pick_next_task_fair() fails to pick a task and the runqueue is about to go idle. The fair scheduler was updating and syncing the PELT clock prematurely, before the previous scheduling class had a chance to update its own PELT signals, resulting in incorrect utilization averages for other scheduling classes.

## Root Cause

The update_idle_rq_clock_pelt() function was being called in pick_next_task_fair() when returning NULL (indicating no task to run). This happens before the previous task's scheduling class (e.g., RT class) gets to call its put_prev_task hook and update its PELT signals. The premature clock sync causes incorrect accounting of idle time and disrupts the PELT data for other scheduling classes that have higher priority.

## Fix Summary

The fix moves the update_idle_rq_clock_pelt() call from pick_next_task_fair() in fair.c to set_next_task_idle() in idle.c. This ensures the PELT clock update happens after all scheduling classes (including the previous task's class) have had the opportunity to update their PELT signals via their put_prev_task hooks, restoring correct utilization accounting.

## Triggering Conditions

The bug occurs during the transition from a higher-priority scheduling class (RT/DL) to idle when no CFS tasks are available. Specifically:
- A CPU has a running RT or deadline task that finishes execution or blocks
- The fair scheduler's `pick_next_task_fair()` is called but finds no runnable CFS tasks (returns NULL)
- The premature `update_idle_rq_clock_pelt()` call happens before the RT/DL class's `put_prev_task` hook updates its PELT signals
- This race condition causes incorrect util_avg tracking for RT/DL runqueues, as the PELT clock sync occurs with stale scheduling class data
- The issue manifests as util_avg regression in RT runqueue statistics observed over time

## Reproduce Strategy (kSTEP)

Setup 2+ CPUs and create RT tasks that will transition the system to idle state:
- Use `kstep_task_create()` and `kstep_task_fifo()` to create RT tasks on CPU 1
- Pin RT tasks with `kstep_task_pin(task, 1, 1)` to ensure they run on specific CPU
- Have RT tasks run briefly then block/finish to trigger the idle transition
- Monitor RT runqueue util_avg before and after using custom callbacks in `on_tick_end()`
- Access RT runqueue via `cpu_rq(1)->rt.rt_rq` and check `rt_rq->avg.util_avg` values
- Use `kstep_tick_repeat()` to advance time and observe PELT signal degradation
- Compare util_avg values: buggy kernels show incorrect/stale values during idle transitions
- Log util_avg changes with `TRACE_INFO()` to detect when PELT clock sync happens prematurely
