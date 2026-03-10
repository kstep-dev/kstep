# sched_ext: Fix scx_bpf_dsq_peek() with FIFO DSQs

- **Commit:** 71d7847cad4475f1f795c7737e08b604b448ca70
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

When removing a task from a FIFO dispatch queue (DSQ), the `task_unlink_from_dsq()` function updates the `dsq->first_task` pointer after the task is still in the list. This causes the subsequent lookup to re-read the same task, leaving `first_task` pointing to the removed entry instead of the actual next task. This bug only affects DSQs operating in FIFO mode, as priority DSQs (which use rbtrees) correctly update the data structure before re-evaluating the first task.

## Root Cause

The original code order was: (1) erase from rbtree if applicable, (2) refresh `first_task` by calling `nldsq_next_task()`, (3) delete from list. When step (2) executes, the task `p` is still in the list, so `nldsq_next_task()` finds and returns `p` itself instead of the next task, causing `first_task` to be assigned the same task that is being unlinked.

## Fix Summary

The fix reorders the operations to delete the task from the list before refreshing `first_task`. This ensures that when `nldsq_next_task()` searches for the next first task, it skips over the removed task and finds the actual next task in the queue.

## Triggering Conditions

This bug occurs in the sched_ext subsystem when:
- A sched_ext BPF scheduler is loaded and active
- Tasks are queued in a FIFO dispatch queue (DSQ) with `dsq->id` not having `SCX_DSQ_FLAG_BUILTIN` set
- The task being dequeued is currently the `dsq->first_task` (head of the queue)
- The DSQ operates in FIFO mode (not using priority rbtree with `SCX_TASK_DSQ_ON_PRIQ`)
- A `scx_bpf_dsq_peek()` or similar operation accesses `first_task` after the buggy unlink
- Race condition where `nldsq_next_task()` executes while the task is still in the list but being removed

## Reproduce Strategy (kSTEP)

Since sched_ext is not available in standard kernel builds and kSTEP targets core scheduler functionality, direct reproduction requires:
- Build kernel with `CONFIG_SCHED_CLASS_EXT=y` and sched_ext support  
- Create a minimal sched_ext BPF scheduler that uses FIFO DSQs
- Use `kstep_task_create()` to create multiple tasks and queue them in a custom DSQ
- Monitor `dsq->first_task` pointer during `task_unlink_from_dsq()` calls
- Trigger task removal while other tasks remain queued to observe stale pointer
- Use callbacks like `on_tick_begin()` to log DSQ state and detect when `first_task` incorrectly points to removed tasks
- Verify bug by checking if `scx_bpf_dsq_peek()` returns the same task repeatedly instead of advancing through the queue
