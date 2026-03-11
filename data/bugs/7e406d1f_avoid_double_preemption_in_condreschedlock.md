# sched: Avoid double preemption in __cond_resched_*lock*()

- **Commit:** 7e406d1ff39b8ee574036418a5043c86723170cf
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When the kernel is configured with CONFIG_PREEMPTION=y, the `__cond_resched_lock()`, `__cond_resched_rwlock_read()`, and `__cond_resched_rwlock_write()` functions were calling `schedule()` twice: once implicitly via the lock unlock operation (which triggers preemption in preemptible configs) and once explicitly via `preempt_schedule_common()`. This double preemption is incorrect and causes unnecessary overhead.

## Root Cause

The original code unconditionally called `preempt_schedule_common()` when the resched flag was set, without considering that the prior `spin_unlock()` or `read_unlock()`/`write_unlock()` operations already trigger a preemption check in PREEMPT/DYNAMIC_PREEMPT configurations. The unlock operations themselves will invoke the scheduler if needed, making the subsequent explicit call redundant.

## Fix Summary

The fix replaces the explicit `if (resched) preempt_schedule_common()` with `if (!_cond_resched())`. The `_cond_resched()` function is a NOP for preemptible configs (since the unlock already triggered preemption) but provides a preemption point for non-preemptible configurations, correctly handling both cases without double preemption.

## Triggering Conditions

This bug occurs in the core scheduler subsystem when:
- Kernel is configured with CONFIG_PREEMPTION=y or CONFIG_PREEMPT_DYNAMIC=y
- A task holds a spinlock or rwlock and calls `__cond_resched_lock()`, `__cond_resched_rwlock_read()`, or `__cond_resched_rwlock_write()`
- The resched flag is set (i.e., `should_resched(PREEMPT_LOCK_OFFSET)` returns true), indicating a higher priority task needs CPU
- The lock unlock operation (`spin_unlock()`, `read_unlock()`, or `write_unlock()`) triggers the first preemption
- The subsequent explicit call to `preempt_schedule_common()` causes the second unnecessary preemption
- This double scheduling creates performance overhead and incorrect scheduler behavior

## Reproduce Strategy (kSTEP)

To reproduce this bug using kSTEP:
- Use 3+ CPUs (CPU 0 reserved for driver, need CPU 1-2 for tasks)
- In `setup()`: Create two tasks with different priorities using `kstep_task_create()` and `kstep_task_set_prio()`
- Pin high-priority task to CPU 1, low-priority task to CPU 2: `kstep_task_pin(task_high, 1, 1)`, `kstep_task_pin(task_low, 2, 2)`
- In `run()`: Start low-priority task first, let it acquire locks and run: `kstep_task_wakeup(task_low)`, `kstep_tick_repeat(10)`
- Wake high-priority task to trigger preemption: `kstep_task_wakeup(task_high)`
- Use `on_tick_begin()` and `on_tick_end()` callbacks to monitor preemption events with `kstep_output_curr_task()`
- Create lock contention by having both tasks compete for shared resources
- Monitor for double `schedule()` calls in preemptible kernel configurations by instrumenting the conditional reschedule functions
- Compare behavior between buggy kernel (double preemption) vs fixed kernel (single preemption)
