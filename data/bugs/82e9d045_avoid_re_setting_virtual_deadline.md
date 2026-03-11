# sched/fair: Avoid re-setting virtual deadline on 'migrations'

- **Commit:** 82e9d0456e06cebe2c89f3c73cdbc9e3805e9437
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/features.h
- **Subsystem:** CFS

## Bug Description

During migration operations such as dequeue-enqueue cycles (e.g., setting task nice value, changing preferred NUMA node, or other task property changes), the CFS scheduler was incorrectly re-setting the virtual deadline to a fresh value. This caused tasks to lose their accumulated deadline slack across these non-critical transitions, leading to improper scheduling fairness and potentially unfair CPU time distribution among tasks.

## Root Cause

The `place_entity()` function always recalculates the deadline from scratch without distinguishing between initial task placement and task migration/re-enqueuing. When a task is dequeued and re-enqueued (not due to sleep), its deadline was unconditionally reset based on its current vruntime, discarding the relative deadline information that should be preserved across such transitions.

## Fix Summary

The fix introduces a new scheduler feature `PLACE_REL_DEADLINE` that preserves the relative deadline across dequeue-enqueue cycles. When dequeuing a task (that is not sleeping), the deadline is converted to a relative form by subtracting vruntime and marking it for preservation. When the task is re-enqueued, the saved relative deadline is restored by adding back the new vruntime, ensuring scheduling fairness is maintained across task migrations.

## Triggering Conditions

This bug is triggered during any dequeue-enqueue operation that is NOT a sleep. The CFS scheduler's `place_entity()` function incorrectly resets the virtual deadline without distinguishing between initial placement and migrations. Operations that trigger this include: changing task nice values via `setpriority()`, modifying preferred NUMA nodes via `set_mempolicy()`, task cgroup migrations, and other property changes. The task must have accumulated deadline slack (deadline != vruntime + slice) and be actively scheduled (not newly forked). The bug manifests when the task's relative deadline position is lost, causing unfair scheduling behavior as tasks lose their accumulated fairness credits across these operations.

## Reproduce Strategy (kSTEP)

Use at least 2 CPUs (CPU 0 reserved). Create two tasks with different nice values in `setup()`. In `run()`, wake both tasks and let them accumulate different deadline positions using `kstep_tick_repeat(50)`. While one task is running, change the other task's nice value using `kstep_task_set_prio()` to trigger dequeue-enqueue. Capture deadline values before/after the nice change using `on_tick_begin()` callback to log `task->se.deadline` and `task->se.vruntime`. The bug is detected when the relative deadline (`deadline - vruntime`) is reset to a fresh value instead of being preserved. Compare deadline relationships before and after the migration - tasks should maintain their relative deadline positions, but the buggy kernel will reset them to new calculated values.
