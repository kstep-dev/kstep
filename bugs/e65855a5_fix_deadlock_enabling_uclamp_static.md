# Fix a deadlock when enabling uclamp static key

- **Commit:** e65855a52b479f98674998cb23b21ef5a8144b04
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When setting a uclamp value for a task via `__sched_setscheduler()`, the code calls `static_key_enable()` while holding scheduler locks. This triggers a deadlock because `static_key_enable()` internally calls `cpus_read_lock()`, which is a blocking operation that cannot be executed from within a non-sleeping context protected by scheduler locks.

## Root Cause

The `static_branch_enable(&sched_uclamp_used)` call was placed in `__setscheduler_uclamp()`, which executes within the critical section protected by scheduler locks. Since `static_key_enable()` calls `cpus_read_lock()` (a percpu rwsem that can sleep), this violates the constraint that code holding scheduler locks must not sleep, resulting in a "BUG: sleeping function called from invalid context" error.

## Fix Summary

The fix moves the `static_branch_enable(&sched_uclamp_used)` call from `__setscheduler_uclamp()` to `uclamp_validate()`, which is invoked earlier in `__sched_setscheduler()` before any scheduler locks are acquired. This ensures the potentially-blocking static key enable operation happens in a safe context while still validating uclamp attributes early enough.

## Triggering Conditions

This deadlock occurs when setting uclamp (utilization clamping) attributes for a task via `__sched_setscheduler()` system call with `SCHED_FLAG_UTIL_CLAMP_MIN` or `SCHED_FLAG_UTIL_CLAMP_MAX` flags. The critical requirement is that the `sched_uclamp_used` static key must be disabled initially, triggering the first-time enable path. The race happens when `__setscheduler_uclamp()` calls `static_branch_enable()` while holding the task's pi_lock and rq lock, causing `cpus_read_lock()` to sleep in atomic context. This can be triggered by any userspace process calling `sched_setattr()` with uclamp values when uclamp hasn't been used system-wide yet.

## Reproduce Strategy (kSTEP)

This bug requires direct kernel API access that kSTEP doesn't expose. A reproduction would need:
- Setup: 2 CPUs minimum, ensure `sched_uclamp_used` static key is initially disabled
- In `setup()`: Create a task with `kstep_task_create()` and use `kstep_task_pin()` to pin it
- In `run()`: Directly call `sys_sched_setattr()` with valid uclamp parameters via kernel module code outside kSTEP APIs
- Use `on_tick_begin()` callback to monitor for "sleeping function called from invalid context" kernel warnings
- Detection: Check for BUG/WARN messages in kernel logs about sleeping in atomic context during static key enable
- Alternative: Hook into `static_branch_enable()` to detect calls while holding scheduler locks via custom callback
