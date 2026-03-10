# sched/eevdf: Fix heap corruption more

- **Commit:** d2929762cc3f85528b0ca12f6f63c2a714f24778
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair Scheduling), EEVDF

## Bug Description

When reweighting a task's weight in the CFS scheduler, the code was calling `min_deadline_cb_propagate()` on all entities with `se->on_rq == true`. However, the current entity (the task currently executing on the CPU) has `on_rq == true` but is not actually stored in the RB tree—it is tracked separately. Calling `min_deadline_cb_propagate()` on the current entity causes it to walk invalid RB tree pointers via `rb_parent()`, leading to heap corruption and KASAN complaints.

## Root Cause

The code assumed that `se->on_rq == true` implies the entity is in the RB tree and safe to pass to tree-manipulation functions. However, the current entity breaks this invariant: it is marked as on the runqueue but is not in the tree structure itself. The previous fix (8dafa9d0eb1a) did not account for this special case.

## Fix Summary

The fix adds a check `if (se != cfs_rq->curr)` before calling `min_deadline_cb_propagate()`, ensuring that the propagation only occurs for entities actually in the tree. The current entity is excluded because it does not reside in the RB tree structure and does not require this heap integrity maintenance.

## Triggering Conditions

This bug is triggered during task weight reweighting in the CFS scheduler when:
- A task is currently executing on CPU (marked as `cfs_rq->curr`) and has `se->on_rq == true`
- The task's weight is changed through reweight_entity() (e.g., via nice value changes or cgroup weight adjustments)
- The task remains the current entity when reweight_entity() is called
- The code path reaches `min_deadline_cb_propagate(&se->run_node, NULL)` for the current entity
- This causes RB tree traversal via `rb_parent()` on an entity not actually in the tree
- Invalid pointer dereferences occur, leading to heap corruption detectable by KASAN

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create a task that becomes current on CPU 1, then trigger weight reweighting:
- In setup(): Create a single task pinned to CPU 1 using `kstep_task_create()` and `kstep_task_pin(task, 1, 1)`
- In run(): Wake the task with `kstep_task_wakeup(task)` and advance scheduler ticks with `kstep_tick_repeat(5)` until task becomes current
- Trigger weight change using `kstep_task_set_prio(task, new_prio)` while task is current on CPU 1
- Use on_tick_begin() callback to monitor when task becomes `cfs_rq->curr` and log heap corruption
- Detect the bug through KASAN complaints in kernel logs or by instrumenting `min_deadline_cb_propagate()` to check if called on current entity
- Compare behavior on buggy vs. fixed kernels to confirm heap corruption is eliminated
