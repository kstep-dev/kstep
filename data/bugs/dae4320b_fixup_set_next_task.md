# sched: Fixup set_next_task() implementations

- **Commit:** dae4320b29f0bbdae93f7c1f6f80b19f109ca0bc
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/fair.c
- **Subsystem:** Deadline, CFS

## Bug Description

The scheduler violates the abstraction rule that `pick_next_task() := pick_task() + set_next_task(.first = true)`. Critical side effects like high-resolution timer (hrtick) initialization, misfit status updates, and stop tick configuration were missing from `set_next_task()` when called with `first=true`. This causes these essential task state initializations to be skipped if `set_next_task(first=true)` is called independently from `pick_task()`, violating the expected behavior contract.

## Root Cause

The code was structured such that `pick_next_task()` performed side effects (hrtick setup, misfit updates, stop tick) inline, but these were not reflected in the `set_next_task()` functions. This created a semantic mismatch: if the abstraction rule is followed elsewhere in the code (using `pick_task()` + `set_next_task(first=true)` instead of `pick_next_task()` directly), the missing side effects would not execute, leading to incomplete task initialization.

## Fix Summary

The fix moves all side effects from `pick_next_task()` into the `set_next_task()` functions, executed when `first=true`. In deadline.c, hrtick setup moves into `set_next_task_dl()`. In fair.c, a new `__set_next_task_fair()` helper encapsulates the `first=true` side effects (hrtick, misfit status, stop tick), and `set_next_task_fair()` calls it to ensure consistent behavior. This ensures the abstraction rule is maintained and all code paths that pick a new task properly initialize its state.

## Triggering Conditions

This bug is triggered when scheduler code uses the decomposed task picking pattern (`pick_task()` + `set_next_task(first=true)`) instead of calling `pick_next_task()` directly. The missing side effects occur during task switching in both deadline and CFS schedulers. For deadline tasks, hrtick timer initialization is skipped when `set_next_task_dl()` is called with `first=true`. For CFS tasks, misfit status updates, stop tick configuration, and other task state initializations are omitted. This happens during any context switch where the alternative task picking path is used, particularly in SMP load balancing scenarios or when task groups are involved.

## Reproduce Strategy (kSTEP)

The bug can be reproduced by forcing the scheduler to use the decomposed task picking path and observing missing hrtick setup for deadline tasks. Create a deadline task on CPU 1 with `kstep_task_create()` and use `sched_setscheduler()` to configure it as SCHED_DEADLINE. Use `kstep_task_wakeup()` to make it runnable, then trigger a scenario where `pick_task()` + `set_next_task(first=true)` is used instead of `pick_next_task()` directly. Monitor hrtick timer state in `on_tick_begin()` callback using `hrtimer_active()` on the CPU's hrtick timer. In the buggy kernel, the timer will not be properly initialized when the decomposed path is taken. For verification, compare with fixed kernel where hrtick is correctly started. The setup needs at least 2 CPUs to allow task placement on CPU 1 while driver runs on CPU 0.
