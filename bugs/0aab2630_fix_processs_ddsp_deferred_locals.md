# sched_ext: Fix processs_ddsp_deferred_locals() by unifying DTL_INVALID handling

- **Commit:** 0aab26309ee9ceafcd5292b81690ccdac0796803
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

The function `process_ddsp_deferred_locals()` calls `dispatch_to_local_dsq()` but fails to handle the `DTL_INVALID` return value, which occurs when a task cannot be dispatched to the target local DSQ due to cpus_allowed restrictions. In this case, the task is not enqueued anywhere, causing it to be lost. The function `finish_dispatch()` correctly handles this case by falling back to the global DSQ, but `process_ddsp_deferred_locals()` does not, creating an inconsistency in task dispatch behavior.

## Root Cause

The `dispatch_to_local_dsq()` function returns three status codes: DTL_DISPATCHED, DTL_LOST, and DTL_INVALID. The DTL_INVALID case indicates that the task cannot be dispatched to the requested local DSQ and must fall back to the global DSQ. However, the caller `process_ddsp_deferred_locals()` ignores this return value entirely, leaving tasks stranded without proper fallback handling. The fix unifies handling by moving the global DSQ fallback into `dispatch_to_local_dsq()` itself, ensuring it happens automatically for all callers.

## Fix Summary

The function `dispatch_to_local_dsq()` is changed from returning an enum to returning void. The DTL_INVALID fallback case (when task cannot run on the destination remote rq) now directly enqueues the task to the global DSQ within the function. This ensures consistent behavior across all callers by guaranteeing that tasks are never lost due to dispatch failures.

## Triggering Conditions

The bug occurs in the sched_ext subsystem when `process_ddsp_deferred_locals()` attempts to dispatch a task to a local DSQ but the task has CPU affinity restrictions that prevent it from running on the destination CPU. This requires:
- sched_ext enabled with local DSQ dispatch operations
- A task with restricted cpus_allowed that excludes the target CPU
- The `process_ddsp_deferred_locals()` code path being triggered during deferred dispatch processing
- The `dispatch_to_local_dsq()` function returning DTL_INVALID due to CPU affinity constraints
- No fallback handling in the caller, causing the task to be lost in the dispatch queues

## Reproduce Strategy (kSTEP)

This bug requires sched_ext support which is not typically available in standard kSTEP configurations. However, if sched_ext were available, reproduction would involve:
- Setup 2+ CPUs with one CPU having restricted access for a specific task
- Create a task with restricted CPU affinity using `kstep_task_create()` and `kstep_task_pin()`
- Trigger sched_ext local DSQ dispatch operations through custom sched_ext operations
- Use `on_tick_begin()` and `on_sched_softirq_end()` callbacks to monitor DSQ states
- Monitor task states to detect when tasks become "lost" (not in any runqueue or DSQ)
- Verify task dispatch failures by checking task_can_run_on_remote_rq() conditions
- Detection involves checking that tasks are neither on local DSQs nor the global DSQ after dispatch attempts
