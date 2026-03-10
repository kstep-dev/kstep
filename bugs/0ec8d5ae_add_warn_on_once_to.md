# sched/core: Add WARN_ON_ONCE() to check overflow for migrate_disable()

- **Commit:** 0ec8d5aed4d14055aab4e2746def33f8b0d409c3
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When repeated migrate_disable() calls are made without corresponding migrate_enable() calls, the unsigned short 'migration_disabled' counter can overflow. In PREEMPT_RT kernels, this overflow causes migrate_disable() to become ineffective, potentially triggering unintended CPU migration and causing RCU read-side critical section nesting counts to leak when local_lock_irqsave() and local_unlock_irqrestore() execute on different CPUs.

## Root Cause

The 'migration_disabled' field is an unsigned short with a maximum value of 65535. When migrate_disable() is called repeatedly without matching migrate_enable() calls, the counter increments past this limit and wraps around, effectively disabling the migration protection mechanism. The absence of overflow detection makes this subtle bug difficult to diagnose.

## Fix Summary

The fix adds WARN_ON_ONCE() checks to detect overflow conditions by casting 'migration_disabled' to a signed short and checking for negative values or overflow. In migrate_disable(), it warns when about to overflow; in migrate_enable(), it warns when overflow has already occurred. This diagnostic enhancement helps developers quickly identify the root cause of bugs caused by missing migrate_enable() calls.

## Triggering Conditions

The bug requires:
- A task that repeatedly calls migrate_disable() without matching migrate_enable() calls
- The migration_disabled counter must reach 65535 and wrap around to 0 (unsigned short overflow)
- On PREEMPT_RT kernels with CONFIG_DEBUG_PREEMPT disabled, the overflow detection is absent
- Once overflowed, the task becomes migrable despite migrate_disable() calls, allowing scheduler to move it between CPUs
- If local_lock_irqsave()/local_unlock_irqrestore() pairs execute on different CPUs due to migration, RCU read-side critical section nesting counts leak
- The bug manifests as RCU warnings like "rcu_note_context_switch" when the task migrates unexpectedly

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (driver uses CPU 0). In setup(), use kstep_task_create() to create a worker task. In run(), pin the task to CPU 1 with kstep_task_pin(). Create a loop that calls an internal migrate_disable() function 65536+ times without migrate_enable() to trigger overflow. Use kstep_tick() between calls to allow scheduling. Monitor task->migration_disabled field directly through kernel symbols or add custom logging. Verify overflow by checking if the counter wraps to 0. Then attempt task migration through load balancing by creating CPU imbalance with additional tasks on CPU 2. Use on_tick_begin() callback to log current CPU of the overflowed task. The bug is triggered if the task migrates despite having called migrate_disable(), which can be detected by observing the task running on different CPUs.
