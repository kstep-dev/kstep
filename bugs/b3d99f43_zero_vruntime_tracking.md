# sched/fair: Fix zero_vruntime tracking

- **Commit:** b3d99f43c72b56cf7a104a364e7fb34b0702828b
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When a single task is running on a CPU, the zero_vruntime tracking mechanism fails to update, causing entity_key() to grow indefinitely large and eventually overflow. This results in incorrect scheduling decisions and can cause severe performance degradation. The test case demonstrates that zero_vruntime grows from ~31k to ~382k over just a few observations on the buggy kernel, whereas the fixed kernel keeps it bounded around ~17k-18k.

## Root Cause

The bug occurs because update_zero_vruntime() was being called only from __enqueue_entity() and __dequeue_entity(). When a single task is running, pick_next_task() always returns that task, and put_prev_set_next_task() never calls either enqueue or dequeue function, so zero_vruntime never gets updated. Additionally, calling avg_vruntime() from within enqueue/dequeue creates accounting inconsistencies: set_next_entity() dequeues before updating cfs_rq->curr (missing the entity in accounting), and put_prev_entity() enqueues before clearing cfs_rq->curr (causing double accounting).

## Fix Summary

The fix relocates zero_vruntime update logic into the avg_vruntime() function itself, so it updates as a side effect whenever avg_vruntime() is called. Additionally, entity_tick() now explicitly calls avg_vruntime() to ensure zero_vruntime is updated on every tick, even when no enqueue/dequeue operations occur. This guarantees zero_vruntime tracking remains consistent regardless of whether there are one or multiple tasks running.

## Triggering Conditions

The bug requires a single CFS task running alone on a CPU for an extended period. The zero_vruntime field grows unbounded because update_zero_vruntime() is never called when there are no enqueue/dequeue operations. Key conditions:
- Only one runnable task on a CPU (no other tasks to enqueue/dequeue)
- Task runs continuously for many ticks without yielding or being preempted
- CFS fair scheduling class (not RT/DL tasks)
- The entity_key() calculation eventually overflows due to growing zero_vruntime
- Observable through /sys/kernel/debug/sched/debug as zero_vruntime increases dramatically
- No specific CPU topology or cgroup setup required - affects any single-task scenario

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create single CFS task on CPU 1 and observe zero_vruntime growth:
- Setup: `kstep_task_create()` one task, `kstep_task_pin()` to CPU 1
- Run: `kstep_task_wakeup()` the task, then `kstep_tick_repeat(100)` for extended runtime
- Monitor: Use `on_tick_end()` callback to periodically check `cpu_rq(1)->cfs.zero_vruntime`
- Detection: Log zero_vruntime values - buggy kernel shows unbounded growth (31k→382k), fixed kernel stays bounded (~17k-18k)
- Alternative: Check entity_key() for the running task to detect overflow conditions
- Validation: Compare zero_vruntime progression between buggy and fixed kernels to confirm reproduction
