# sched_ext: Fix starvation of scx_enable() under fair-class saturation

- **Commit:** b06ccbabe2506fd70b9167a644978b049150224a
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

During `scx_enable()`, the calling thread's sched_class is switched from fair to ext. Since fair class has higher priority than ext class, saturating the system with fair-class workloads can indefinitely starve the enable thread, causing it to hang in the READY → ENABLED task switching loop and ultimately hanging the entire system. The earlier protection using `preempt_disable()` was incomplete—in partial switch modes, the thread could still be starved after `preempt_enable()`.

## Root Cause

When `scx_enable()` was changed from using `preempt_disable()` to `scx_bypass()`, the protection against fair-class starvation was lost. The fair scheduler has higher priority than the ext scheduler, so when the calling thread switches its class from fair to ext during enable, it becomes vulnerable to being starved indefinitely by fair-class tasks on the runqueue. This is a priority inversion problem where a lower-priority ext-class thread cannot make progress against higher-priority fair-class contenders.

## Fix Summary

The fix offloads the entire `scx_enable()` body to a dedicated system-wide RT (SCHED_FIFO) kthread that is created lazily on first use. RT class has higher priority than both fair and ext classes, so the kthread cannot be starved regardless of system load. The main `scx_enable()` function synchronously waits for the kthread to complete using `kthread_flush_work()`.

## Triggering Conditions

The bug occurs during the sched_ext enablement process when:
- A userspace thread calls `scx_enable()` to activate a sched_ext scheduler
- During the READY → ENABLED state transition loop, the calling thread's sched_class switches from fair to ext
- The system runqueue is saturated with fair-class tasks that can preempt the enable thread
- Fair class has higher priority than ext class, creating priority inversion where the lower-priority ext-class enable thread cannot make progress
- The enable thread gets indefinitely starved and hangs in the task switching loop
- This leads to system hang as the sched_ext activation cannot complete

## Reproduce Strategy (kSTEP)

This bug is specific to sched_ext subsystem which is not currently supported by kSTEP. To reproduce a similar priority inversion scenario with available APIs:
- **CPUs needed**: 2+ CPUs (CPU 0 reserved for driver)
- **Setup**: Create multiple fair-class tasks to saturate the system: `kstep_task_create()` for 4-8 tasks, ensure they are CFS with `kstep_task_cfs()`
- **Trigger**: Pin saturating tasks to CPUs 1-N with `kstep_task_pin()`, then `kstep_task_wakeup()` to activate them
- **Load**: Use `kstep_tick_repeat(100)` to let fair tasks consume CPU cycles and build saturation
- **Observe**: Monitor runqueue state with `on_tick_begin` callback and `kstep_output_nr_running()` to confirm fair-class saturation
- **Detection**: Look for signs of starvation in lower-priority tasks (though full reproduction requires sched_ext support in kSTEP framework)
