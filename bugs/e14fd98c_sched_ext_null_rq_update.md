# sched/ext: Prevent update_locked_rq() calls with NULL rq

- **Commit:** e14fd98c6d66cb76694b12c05768e4f9e8c95664
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

When SCX_CALL_OP and SCX_CALL_OP_RET macros are invoked from unlocked contexts, they pass NULL as the rq parameter to update_locked_rq(). This triggers a kernel warning "BUG: using __this_cpu_write() in preemptible context" because __this_cpu_write() is unsafe to use when preemption is enabled and the rq is NULL.

## Root Cause

The update_locked_rq() function uses __this_cpu_write() to store the rq pointer, which is not safe in preemptible context. When ops are invoked from unlocked contexts, rq is NULL, but the macros were unconditionally calling update_locked_rq(NULL) regardless, triggering the warning about unsafe per-CPU operations in preemptible context.

## Fix Summary

The fix adds conditional checks before calling update_locked_rq() in both SCX_CALL_OP and SCX_CALL_OP_RET macros. update_locked_rq() is now only called when rq is non-NULL, preventing __this_cpu_write() from being invoked in preemptible context when the rq is unlocked.

## Triggering Conditions

This bug occurs specifically in the sched_ext subsystem when SCX_CALL_OP or SCX_CALL_OP_RET macros invoke scheduler operations from unlocked contexts. The trigger requires: (1) sched_ext to be enabled with an active SCX scheduler, (2) SCX ops callbacks being invoked from contexts where no runqueue is locked (rq=NULL), (3) preemption enabled during the callback invocation, and (4) the callback path executing update_locked_rq(NULL) which uses __this_cpu_write() in preemptible context. This typically happens when SCX operations are called from process context rather than scheduler context, such as during scheduler initialization, teardown, or certain administrative operations where locks are not held.

## Reproduce Strategy (kSTEP)

Reproducing this bug requires sched_ext support, which may not be directly available through standard kSTEP APIs. However, the reproduction would involve: (1) enabling sched_ext with a custom SCX scheduler that has callbacks triggering from unlocked contexts, (2) using 2+ CPUs where CPU 0 runs the driver and CPU 1+ run test workloads, (3) in setup(), enable preemption debugging and configure sched_ext if available, (4) in run(), trigger SCX operations that invoke callbacks from process context (not holding rq locks), such as scheduler switching or certain administrative operations, (5) use kstep_task_create() and kstep_task_wakeup() to create scheduler activity, (6) monitor for "BUG: using __this_cpu_write() in preemptible" warnings in kernel logs. Detection would involve checking dmesg output or kernel warning logs for the specific __this_cpu_write() preemptible context warning message.
