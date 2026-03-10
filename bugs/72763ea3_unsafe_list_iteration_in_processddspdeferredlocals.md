# sched_ext: Fix unsafe list iteration in process_ddsp_deferred_locals()

- **Commit:** 72763ea3d45c7f9fd69b825468afbf4d11c5ffc2
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The `process_ddsp_deferred_locals()` function iterates through a list of deferred direct dispatch tasks and calls `dispatch_to_local_dsq()` on each. However, `dispatch_to_local_dsq()` can temporarily drop the rq lock, allowing the list to be modified during iteration. This can cause list corruption, leading to oopses and unpredictable failures.

## Root Cause

The function used `list_for_each_entry_safe()` to iterate the list while holding the rq lock. Although `list_for_each_entry_safe()` saves a next pointer to handle deletion, it cannot protect against the list being modified when the lock is dropped inside `dispatch_to_local_dsq()`. The saved pointer can become invalid, causing the iterator to access freed or moved list nodes.

## Fix Summary

Changed from `list_for_each_entry_safe()` to a `while` loop that pops entries from the list head using `list_first_entry_or_null()`. This avoids iterator corruption by always fetching the current head of the list after each operation, ensuring that modifications made during the lock-dropping call are safely handled.

## Triggering Conditions

This bug affects the sched_ext subsystem specifically in the `process_ddsp_deferred_locals()` code path. The triggering conditions require:
- Kernel built with CONFIG_SCHED_CLASS_EXT=y and sched_ext BPF scheduler loaded
- Tasks being directly dispatched to local DSQs of remote CPUs (SCX_DSQ_LOCAL_ON dispatch)
- Multiple tasks queued in `rq->scx.ddsp_deferred_locals` list for processing
- Concurrent operations that modify the deferred locals list while `dispatch_to_local_dsq()` temporarily drops the rq lock
- Race condition where the saved 'next' pointer in `list_for_each_entry_safe()` becomes stale due to list modifications during lock dropping
- The bug manifests as list corruption, potentially causing oopses when the iterator accesses freed or moved nodes

## Reproduce Strategy (kSTEP)

This bug requires sched_ext functionality not directly exposed in kSTEP's current API. A reproduction strategy would need:
- Multi-CPU setup (at least 3 CPUs: CPU 0 for driver, CPU 1-2 for tasks)
- Extension of kSTEP to support sched_ext operations or direct kernel manipulation
- Create multiple tasks that trigger direct dispatch to remote CPU's local DSQ
- Use timing manipulation to create the race condition where `dispatch_to_local_dsq()` drops locks
- Monitor for list corruption via custom callbacks during deferred dispatch processing
- Detection method: Log list state before/after `process_ddsp_deferred_locals()` calls, check for kernel oopses
- Since current kSTEP lacks sched_ext support, reproduction would require either extending the framework with sched_ext APIs or using direct kernel symbol access to manipulate `rq->scx.ddsp_deferred_locals` lists
