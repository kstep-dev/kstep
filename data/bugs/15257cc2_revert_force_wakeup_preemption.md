# sched/fair: Revert force wakeup preemption

- **Commit:** 15257cc2f905dbf5813c0bfdd3c15885f28093c4
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The scheduler aggressively forces preemption when the waker task becomes ineligible, bypassing run-to-parity and slice protection mechanisms. However, this does not guarantee the wakee will run next, leading to incorrect task execution order. This causes excessive rescheduling and preemption because tasks quickly become ineligible before exhausting their time slice or quantum.

## Root Cause

The problematic code (removed in this fix) forced preemption when the waker becomes ineligible under the assumption this is what the waker wants, but the scheduler cannot guarantee this will result in the wakee actually running next. With periodic vruntime updates, a task that has only run briefly can become ineligible before using its full slice, triggering forced preemption that disrupts the intended scheduling order and violates EEVDF (Earliest Eligible Virtual Deadline First) protections.

## Fix Summary

Removes the force preemption logic that reschedules when the waker becomes ineligible. Instead, the scheduler relies on proper use of yield_to_task or WF_SYNC hints for explicit preemption requests, allowing the normal NEXT_BUDDY and scheduling protections to handle task ordering correctly.

## Triggering Conditions

The bug occurs in `check_preempt_wakeup_fair()` when a waker task becomes ineligible after waking another task. Specific conditions:
- Two or more tasks (A, B) wake simultaneously with similar vruntime/lag values (both eligible)
- Task A runs first and performs a wakeup operation on task C
- During A's execution, `update_curr()` advances A's vruntime beyond the average, making A ineligible
- A has not exhausted its time slice or minimum quantum, but becomes ineligible due to vruntime advancement
- The removed code `!entity_eligible(cfs_rq, se)` check triggers forced preemption (PREEMPT_WAKEUP_RESCHED)
- Despite forcing preemption to favor wakee C, scheduler picks task B instead, violating intended wake-up ordering

## Reproduce Strategy (kSTEP)

Use 2 CPUs (CPU 0 reserved for driver, use CPU 1). Create three CFS tasks A, B, C with equal priority.
In `setup()`: Create tasks with `kstep_task_create()` for A, B, C.
In `run()`: 
1. Wake A and B simultaneously with `kstep_task_wakeup()` to establish similar vruntime baselines
2. Run several ticks with `kstep_tick_repeat()` until A becomes current
3. Create task C but keep it paused initially  
4. Let A run briefly to advance its vruntime, then have A wake up C via `kstep_task_wakeup(C)`
5. Use `on_tick_begin()` callback to monitor when A becomes ineligible with `kstep_eligible()`
6. Log scheduler decisions and observe if forced preemption occurs but B runs instead of C
7. Compare task execution order: bug present if C doesn't run next despite being recently awakened by A
Detect bug by checking if excessive preemption events occur when waker becomes ineligible without proper wakee scheduling.
