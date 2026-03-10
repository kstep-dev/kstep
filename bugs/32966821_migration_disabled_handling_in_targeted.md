# sched_ext: Fix migration disabled handling in targeted dispatches

- **Commit:** 32966821574cd2917bd60f2554f435fe527f4702
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

When using targeted dispatch operations (scx_bpf_dsq_move_to_local() or scx_bpf_dsq_move()), a task that is already on the target CPU and has migration disabled is incorrectly bounced to a global DSQ instead of being dispatched locally. The task_can_run_on_remote_rq() function returns false when it shouldn't, causing unnecessary task migration through the global queue.

## Root Cause

The function task_can_run_on_remote_rq() assumes that the task is on a different CPU than the target CPU, but the callers (move_task_between_dsqs() and dispatch_to_local_dsq()) do not enforce this precondition. When a task with migration disabled is already on the target CPU, the is_migration_disabled() check incorrectly triggers, causing the function to return false and the task to be routed through the global DSQ instead of the intended local DSQ.

## Fix Summary

The fix adds explicit src_rq != dst_rq checks before calling task_can_run_on_remote_rq() in both move_task_between_dsqs() and dispatch_to_local_dsq() functions. Additionally, SCHED_WARN_ON() assertions are added to task_can_run_on_remote_rq() to catch any future violations of its CPU difference assumption, and the is_migration_disabled() check is converted to a warning rather than a functional guard since it should be impossible to trigger if the CPU check is enforced.

## Triggering Conditions

The bug occurs in the sched_ext subsystem during targeted dispatch operations (scx_bpf_dsq_move_to_local() or scx_bpf_dsq_move()) when:
- A task has migration disabled (is_migration_disabled() returns true) 
- The task is already running on the target CPU (task_cpu(task) == target_cpu)
- A targeted dispatch to a local DSQ is attempted via move_task_between_dsqs() or dispatch_to_local_dsq()
- These functions call task_can_run_on_remote_rq() without checking if src_rq != dst_rq first
- task_can_run_on_remote_rq() incorrectly returns false due to migration disabled check
- The task gets bounced to a global DSQ instead of staying on the local DSQ
- Requires sched_ext to be enabled and active with BPF scheduler making targeted dispatches

## Reproduce Strategy (kSTEP)

Note: This bug is specific to sched_ext which may not be directly supported by kSTEP. A theoretical approach:
- Setup: 2+ CPUs (CPU 0 reserved for driver), enable sched_ext if supported
- Create task on CPU 1: `task = kstep_task_create(); kstep_task_pin(task, 1, 1);`
- Simulate migration disabled: Use kstep_freeze_task() or internal state manipulation
- Monitor dispatch behavior: Use on_tick_begin() to log task placement and DSQ state  
- Trigger targeted dispatch: Simulate scx_bpf_dsq_move_to_local() by manipulating internal ext.c state
- Detection: Check if task incorrectly moves to global DSQ despite being on target CPU
- Alternative: Create minimal BPF sched_ext scheduler that reproduces the dispatch pattern
- Verify fix: Same test on patched kernel should keep task on local DSQ
