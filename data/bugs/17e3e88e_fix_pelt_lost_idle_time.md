# Fix pelt lost idle time detection

- **Commit:** 17e3e88ed0b6318fde0d1c14df1a804711cab1b5
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair scheduling)

## Bug Description

When `pick_next_task_fair()` is called from the slow-path (with `rf=NULL`) and fails to pick a task because the runqueue is about to go idle, the lost idle time for the PELT clock is not accounted for. This happens specifically when the last running task on the runqueue is an RT or DL task that goes to sleep and the sum of util_sum is at maximum, preventing proper tracking of idle time in the PELT load tracking system.

## Root Cause

The `update_idle_rq_clock_pelt(rq)` call was placed after a check for `if (!rf) return NULL`, meaning it was only executed when `rf` was non-NULL (the fair fast-path). When called from the slow-path with `rf=NULL`, the function would return early without ever calling `update_idle_rq_clock_pelt()`, skipping the necessary idle time tracking.

## Fix Summary

The fix wraps the code block that depends on `rf` being non-NULL (sched_balance_newidle and its related checks) inside an `if (rf)` conditional, while moving the `update_idle_rq_clock_pelt(rq)` call outside this block. This ensures the idle time accounting is always performed before returning NULL, regardless of whether the function was called from the fast-path or slow-path.

## Triggering Conditions

The bug triggers when `pick_next_task_fair()` is called from the slow-path with `rf=NULL` and fails to find a CFS task to schedule. Specifically:
- An RT or DL task is the last running task on a runqueue and goes to sleep
- No CFS tasks are runnable on that CPU (CFS runqueue is empty)
- The sum of util_sum for the runqueue is at maximum value (PELT tracking at saturation)
- The scheduler calls `pick_next_task_fair()` with `rf=NULL` (slow-path scheduling)
- Function reaches the `idle:` label and returns NULL without calling `update_idle_rq_clock_pelt()`
- This causes lost idle time to not be properly accounted in PELT load tracking

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. In setup(), create an RT task and ensure CFS runqueue becomes empty. In run():
1. Use `kstep_task_create()` to create an RT task and pin it to CPU 1 with `kstep_task_pin()`
2. Set task to RT priority using `kstep_task_fifo()` and wake it with `kstep_task_wakeup()`
3. Let the RT task run to saturate PELT util_sum by calling `kstep_tick_repeat()` for extended time
4. Put the RT task to sleep using `kstep_task_pause()` to trigger slow-path scheduling
5. Use `on_tick_begin()` callback to monitor runqueue state and PELT accounting
6. Check for missing idle time updates by logging PELT clock state before/after task sleep
7. Compare PELT idle time tracking on buggy vs fixed kernels to detect the missing updates
