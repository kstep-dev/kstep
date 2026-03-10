# sched_ext: Fix missing post-enqueue handling in move_local_task_to_local_dsq()

- **Commit:** f5e1e5ec204da11fa87fdf006d451d80ce06e118
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

When a task is moved from a non-local dispatch queue (DSQ) to a local DSQ via `move_local_task_to_local_dsq()`, the function directly manipulates the local DSQ without performing post-enqueue handling. This causes preemption to not be triggered when `SCX_ENQ_PREEMPT` is set or when the idle task is running. As a result, when `scx_bpf_dsq_move()` is called while the CPU is busy, task preemption and rescheduling may fail to occur as intended, leading to incorrect task scheduling priority.

## Root Cause

The `move_local_task_to_local_dsq()` function bypasses the standard `dispatch_enqueue()` path to directly add tasks to the local DSQ. However, it was missing the call to `local_dsq_post_enq()`, which handles post-enqueue logic including preemption requests and rescheduling. This means the function didn't respect the `SCX_ENQ_PREEMPT` flag or check whether the idle task was running, causing the system to not reschedule when it should.

## Fix Summary

The fix adds a `local_dsq_post_enq()` call to `move_local_task_to_local_dsq()` after updating the DSQ and task state. Additionally, the fix adds an early exit in `local_dsq_post_enq()` when `SCX_RQ_IN_BALANCE` is set, ensuring that unnecessary reschedules don't occur in the dispatch path while maintaining correct preemption behavior during normal task movement.

## Triggering Conditions

The bug occurs when using sched_ext and calling `scx_bpf_dsq_move()` (which uses `move_local_task_to_local_dsq()`) while a target CPU is busy running a task. The specific conditions needed are:
- A sched_ext scheduler must be loaded and active
- Tasks must exist in non-local dispatch queues (DSQs)
- A task movement from non-local DSQ to local DSQ occurs via `scx_bpf_dsq_move()`
- The target CPU is currently busy (not in balance, not running idle task)
- The move operation has `SCX_ENQ_PREEMPT` flag set or the current task should be preempted
- Without the fix, `local_dsq_post_enq()` is not called, so preemption/rescheduling fails to trigger
- The bug manifests as delayed or missing task preemption, leading to incorrect scheduling priority

## Reproduce Strategy (kSTEP)

**Note: This bug requires sched_ext support, which kSTEP currently lacks. This strategy outlines what would be needed:**

Requires 2+ CPUs (CPU 0 reserved for driver). In `setup()`: enable sched_ext via loading a custom BPF scheduler that creates non-local DSQs and implements `scx_bpf_dsq_move()` operations. Create high-priority and low-priority tasks using `kstep_task_create()` with different priorities via `kstep_task_set_prio()`.

In `run()`: Place low-priority task on CPU 1 using `kstep_task_pin()`, ensure it starts running with `kstep_task_wakeup()` and `kstep_tick()`. Move high-priority task to non-local DSQ, then call `scx_bpf_dsq_move()` with `SCX_ENQ_PREEMPT` to move it to CPU 1's local DSQ while CPU 1 is busy.

Use `on_tick_begin()` callback to monitor current task on CPU 1. Bug detected when high-priority task fails to preempt low-priority task promptly, indicating missing `local_dsq_post_enq()` call. Success when high-priority task preempts immediately after DSQ move.
