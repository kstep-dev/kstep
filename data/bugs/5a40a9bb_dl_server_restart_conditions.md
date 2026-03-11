# sched/debug: Fix dl_server (re)start conditions

- **Commit:** 5a40a9bb56d455e7548ba4b6d7787918323cbaf0
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/debug.c
- **Subsystem:** Deadline (DL server)

## Bug Description

The deadline server fails to (re)start under two conditions: (1) When the server is re-enabled (runtime > 0) after being disabled while tasks are already enqueued, the `is_active` flag remains 0, causing `dl_server_start()` not to be called, leaving the server inactive and causing task starvation. (2) When parameter application fails, the runtime value is not reverted, leaving the system in an inconsistent state where the server won't activate despite a new runtime being set.

## Root Cause

The original code only called `dl_server_start()` if both `is_active` (the previous state) and `runtime` (the new value) were true. This logic fails when transitioning from disabled→enabled because `is_active` reflects the previous state where runtime was 0, not the current intent. Additionally, unconditional application of parameters without checking the return value means failure cases leave stale state that prevents later restart attempts.

## Fix Summary

The fix removes the `is_active` state check and unconditionally calls `dl_server_start()` after every parameter change. The start function is modified to check the actual `dl_runtime` value itself (returning early if runtime is 0), making it safe to call unconditionally. This ensures the server activates whenever it has work to do, regardless of previous state transitions.

## Triggering Conditions

The bug occurs in the deadline server parameter update path (`sched_server_write_common()`) when writing to `/sys/kernel/debug/sched/cpu*/fair_server_runtime`. Two scenarios trigger the bug: (1) Disable the dl_server by setting runtime=0, then re-enable it (runtime>0) while fair tasks are already enqueued on the runqueue - the `is_active` check prevents restart. (2) Parameter validation fails in `dl_server_apply_params()` but runtime isn't reverted, leaving stale state. The bug requires fair/CFS tasks that would benefit from dl_server protection against starvation, and specific timing where server parameter changes occur while tasks are waiting to run.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved). In `setup()`, create multiple CFS tasks with `kstep_task_create()` and `kstep_task_cfs()`. In `run()`, enqueue tasks on CPU 1 with `kstep_task_wakeup()` and let them run with `kstep_tick_repeat()`. Disable the dl_server by writing "0" to `/sys/kernel/debug/sched/cpu1/fair_server_runtime` using `kstep_write()`. Verify tasks continue running. Then re-enable with non-zero runtime (e.g., "50000") while tasks remain enqueued. Monitor dl_server state via `/sys/kernel/debug/sched/cpu1/fair_server_runtime` reads and task scheduling behavior. Use `on_tick_begin()` callback with `kstep_output_curr_task()` to detect if fair tasks get starved (dl_server inactive). Bug triggered if server remains inactive despite positive runtime.
