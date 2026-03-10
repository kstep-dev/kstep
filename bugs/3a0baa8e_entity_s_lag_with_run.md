# sched/fair: Fix entity's lag with run to parity

- **Commit:** 3a0baa8e6c570c252999cb651398a88f8f990b4a
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When a task is enqueued without preempting the currently running task, the slice protection (vprot) mechanism fails to account for the newly enqueued task's slice duration. This causes the task's lag to potentially exceed its allocated slice plus one tick. Under RUN_TO_PARITY scheduling, this violation of the slice guarantee can lead to unfair scheduling and latency issues.

## Root Cause

The `check_preempt_wakeup_fair()` function determines whether to preempt the current task based on EEVDF (Earliest Eligible Virtual Deadline First) criteria. When a shorter-slice task should preempt but the preemption check fails (due to other scheduling conditions), the slice protection of the current task is not adjusted. This means the current task's vprot remains unaware of the minimum slice present in the runqueue, allowing its virtual runtime to advance beyond the protected region without accounting for the newly enqueued task's slice.

## Fix Summary

The fix introduces a new `update_protect_slice()` function that tightens the slice protection by taking into account the minimum slice of all enqueued entities. In `check_preempt_wakeup_fair()`, when RUN_TO_PARITY is enabled and a short preemption is warranted but doesn't occur, `update_protect_slice()` is called to ensure the current task's vprot is constrained by the newly enqueued task's slice. This guarantees that no task's lag will exceed its slice duration plus one tick.

## Triggering Conditions

The bug is triggered when RUN_TO_PARITY scheduling feature is enabled and a shorter-slice task is enqueued without preempting the current task. Specifically:
- A task is currently running with slice protection (vprot) set based on the previous minimum slice
- A new task with a shorter slice duration is woken up and enqueued on the same CPU
- EEVDF's `__pick_eevdf()` determines that short preemption should occur (`do_preempt_short=true`)
- However, the preemption check fails and the current task continues running
- The current task's vprot remains unchanged, not accounting for the newly enqueued shorter slice
- As the current task continues execution, its lag can exceed its original slice + tick guarantee
- This violates the fundamental RUN_TO_PARITY scheduling guarantee

## Reproduce Strategy (kSTEP)

Create two CFS tasks with different slice characteristics and force the problematic scenario:
- Use 2+ CPUs (CPU 0 reserved for driver, test on CPU 1)  
- In `setup()`: Create two tasks A (longer slice) and B (shorter slice) using `kstep_task_create()`
- In `run()`: Pin both tasks to CPU 1 with `kstep_task_pin()`, wake A first and let it run
- Use `kstep_tick_repeat()` to establish A as current with slice protection set
- Wake task B (shorter slice) at a timing that triggers `do_preempt_short` but avoids actual preemption
- Monitor A's lag using on_tick callbacks and check if it exceeds slice + tick duration
- Use `TRACE_INFO()` to log vprot, vruntime, slice values and lag calculations
- Detect bug when A's lag > A's slice + tick_interval without vprot adjustment for B's shorter slice
- Verify fix by ensuring `update_protect_slice()` properly constrains A's execution time
