# sched/hrtick: Fix hrtick() vs. scheduling context

- **Commit:** e38e5299747b23015b00b0109891815db44a2f30
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The `hrtick()` function invokes the task_tick callback of the donor scheduling class, but passes the wrong task as an argument. It passes `rq->curr` (the current task) instead of `rq->donor` (the donor task). When these tasks differ—which can happen with scheduling classes or delegation scenarios—the wrong task's tick callback is invoked, leading to incorrect scheduling decisions, stale state, or improper task accounting.

## Root Cause

The `hrtick()` function uses `rq->donor->sched_class` to determine which scheduling class's task_tick method to call, but then passes `rq->curr` as the task argument instead of `rq->donor`. This creates an inconsistency: the method is called on the donor's scheduling class, but operates on the current task. In contrast, `sched_tick()` correctly passes `rq->donor` as the argument when calling the same method, establishing the expected behavior. When `rq->curr != rq->donor`, the wrong task receives the tick callback.

## Fix Summary

The fix changes the argument from `rq->curr` to `rq->donor` in the `hrtick()` function's call to `task_tick()`. This makes the hrtick path consistent with the regular sched_tick path, ensuring the correct task is passed to the donor scheduling class's task_tick method.

## Triggering Conditions

This bug triggers when `rq->curr != rq->donor` during hrtimer execution. The donor/current task divergence typically occurs with:
- Scheduling class delegation where a donor class (e.g., DL/RT) executes on behalf of another class
- Proxy execution scenarios in core scheduling or bandwidth/throttling contexts  
- Task state transitions during context switches where hrtimer fires between donor assignment and current task update
- Multi-class environments where higher priority classes temporarily donate CPU time
- The hrtimer must fire precisely when this curr/donor mismatch exists
- SCHED_HRTICK must be enabled for the wrong task_tick() path to execute

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). Create tasks with different scheduling classes to trigger donor/curr mismatch:
- In `setup()`: Create RT task and CFS task using `kstep_task_create()`, configure with `kstep_task_fifo()` for RT class
- In `run()`: Pin RT task to CPU 1 with `kstep_task_pin()`, start both tasks with `kstep_task_wakeup()` 
- Enable hrtick via sysctl: `kstep_sysctl_write("kernel.sched_hrtick_enabled", "1")`
- Use `kstep_tick_repeat()` to advance time and trigger task switching patterns that create curr/donor divergence
- In `on_tick_begin()`: Log current CPU's `rq->curr` and `rq->donor` task IDs to detect mismatch
- Monitor task_tick callbacks by adding debug traces in kernel to verify wrong task receives tick
- Detection: Observe task_tick() being called on curr task when donor class method was invoked
