# sched/deadline: Avoid double update_rq_clock()

- **Commit:** 4de9ff76067b40c3660df73efaea57389e62ea7a
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

The function `setup_new_dl_entity()` was calling `update_rq_clock()` unconditionally, but one of its call paths (via `enqueue_task_dl()` → `enqueue_dl_entity()`) already has the rq-clock updated. This causes the clock to be updated unnecessarily twice in the task enqueue path, which is incorrect and can lead to inconsistencies in scheduler state.

## Root Cause

The `update_rq_clock()` call was placed inside `setup_new_dl_entity()` without considering that one caller (`enqueue_dl_entity()`) already ensures the clock is up-to-date before calling it. The other caller, `sched_init_dl_servers()`, does not update the clock before calling `setup_new_dl_entity()`. This mismatch means the clock update needs to be caller-specific rather than happening inside `setup_new_dl_entity()`.

## Fix Summary

The fix removes `update_rq_clock()` from inside `setup_new_dl_entity()` and adds it explicitly in `sched_init_dl_servers()` before the call to `setup_new_dl_entity()`. This ensures each caller handles clock updates appropriately: the enqueue path preserves its existing clock update, while the server initialization path adds its own, avoiding the double update.

## Triggering Conditions

This bug is triggered when deadline tasks are enqueued through the normal task wakeup/enqueue path:
- A deadline task transitions from sleeping/blocked to runnable state
- The enqueue path (`enqueue_task_dl()` → `enqueue_dl_entity()`) calls `update_rq_clock()` to synchronize the runqueue clock
- Subsequently, `setup_new_dl_entity()` is called to initialize the deadline entity for a new scheduling period
- Inside `setup_new_dl_entity()`, `update_rq_clock()` is called again unnecessarily
- This double update can cause scheduler state inconsistencies, particularly with time-sensitive deadline calculations
- The bug affects any deadline task being woken up, not just newly created ones
- Race conditions may amplify the effect when multiple deadline tasks are being enqueued simultaneously

## Reproduce Strategy (kSTEP)

To reproduce this bug using kSTEP framework:
- **CPUs needed**: 2 (CPU 0 reserved for driver, CPU 1 for deadline task)
- **Setup**: Create a deadline task using `kstep_task_create()`, configure it with deadline scheduling policy
- **Reproduction sequence**:
  1. Set up a deadline task and configure deadline parameters (runtime, deadline, period)
  2. Put the task to sleep using `kstep_task_pause()`
  3. Advance time with `kstep_tick_repeat()` to ensure clock progression
  4. Wake up the deadline task using `kstep_task_wakeup()` to trigger the enqueue path
  5. Use `on_tick_begin()` callback to monitor `update_rq_clock()` calls
- **Detection**: Log scheduler clock updates during task enqueue operations, verify `rq_clock()` values before/after `setup_new_dl_entity()`
- **Expected behavior**: In buggy kernel, observe double clock updates; in fixed kernel, single update per enqueue operation
- **Validation**: Check for consistent deadline calculations and proper task timing behavior
