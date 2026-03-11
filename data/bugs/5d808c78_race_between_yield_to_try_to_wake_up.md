# Fix race between yield_to() and try_to_wake_up()

- **Commit:** 5d808c78d97251af1d3a3e4f253e7d6c39fd871e
- **Affected file(s):** kernel/sched/syscalls.c
- **Subsystem:** core

## Bug Description

A race condition exists between `yield_to()` and `try_to_wake_up()` where `yield_to()` can operate on a task without holding the correct runqueue lock. After `yield_to()` acquires the rq locks for the current and target task's runqueues, the target task can be awakened to a different CPU by another context, changing its runqueue, but `yield_to()` continues operating on the stale runqueue without proper synchronization. This causes a SCHED_WARN to trigger in `set_next_buddy()`.

## Root Cause

The race window occurs because `yield_to()` performs a double-checked lock pattern: it locks two runqueues, checks that `task_rq(p) == p_rq`, but between this check and the actual operation, `try_to_wake_up()` can move the task to a different CPU (holding the task's pi_lock), invalidating the locked runqueues. The problem is that `yield_to()` does not hold the task's pi_lock during the critical section, allowing the task's runqueue to change without `yield_to()` detecting it properly before operating on it.

## Fix Summary

The fix acquires the target task's pi_lock (`&p->pi_lock`) at the entry of `yield_to()` via `scoped_guard(raw_spinlock_irqsave, &p->pi_lock)`. This ensures the task's runqueue cannot change due to concurrent `try_to_wake_up()` calls while `yield_to()` holds the pi_lock, eliminating the race window and allowing the double-check pattern to work correctly.

## Triggering Conditions

The race requires precise timing between `yield_to()` and `try_to_wake_up()` on a multi-CPU system:
- Target task initially blocked/sleeping on CPU1, with its task_rq pointing to CPU1's runqueue
- Yielding task on CPU0 calls `yield_to()` on the blocked target task
- `yield_to()` locks both CPU0 and CPU1 runqueues, performs double-check that `task_rq(p) == p_rq`
- After the check passes but before `yield_to_task_fair()` executes, another CPU concurrently calls `try_to_wake_up()` on the target task
- `try_to_wake_up()` migrates the target task from CPU1 to CPU2, updating `task_rq(p)` to CPU2's runqueue
- `yield_to()` proceeds to call `yield_to_task_fair()` while holding stale CPU1 lock instead of required CPU2 lock
- `set_next_buddy()` triggers SCHED_WARN due to operating on task without proper runqueue synchronization

## Reproduce Strategy (kSTEP)

Requires minimum 3 CPUs (CPU 0 reserved for driver). Setup phase:
- Create target task with `kstep_task_create()`, pin to CPU1 with `kstep_task_pin(target, 1, 1)`
- Create yielding task, pin to CPU2 with `kstep_task_pin(yielder, 2, 2)`  
- Create waker kthread with `kstep_kthread_create("waker")`, bind to CPU3 with `kstep_kthread_bind()`
- Start target task, then immediately pause with `kstep_task_pause()` to make it block on CPU1

Execution phase:
- Start waker kthread with `kstep_kthread_start()`, configure it to call `kstep_task_wakeup(target)` after delay
- From yielding task context, call `yield_to()` on the blocked target task
- Use `kstep_tick()` with precise timing to create race window where waker migrates target between `yield_to()`'s double-check and `yield_to_task_fair()`
- Monitor via `on_tick_begin()` callback for SCHED_WARN in `set_next_buddy()` or detect inconsistent runqueue state
- Log target task's `task_rq()` changes and runqueue lock holders to verify race occurred
