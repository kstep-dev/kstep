# sched/eevdf: Fix min_deadline heap integrity

- **Commit:** 8dafa9d0eb1a1550a0f4d462db9354161bc51e0c
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (EEVDF scheduling)

## Bug Description

The min_deadline heap becomes corrupted when cgroups reschedule tasks, causing the scheduler to fail with "EEVDF scheduling fail, picking leftmost" messages. The heap corruption occurs because the deadline value is updated during task reweighting, but this updated value is not propagated up the heap, violating heap invariants and causing incorrect task selection.

## Root Cause

The `reweight_entity()` function optimizes by avoiding the standard dequeue+update+enqueue pattern. However, when it updates the task deadline during reweighting (in the `se->on_rq` path), it modifies `se->deadline` but fails to propagate this change up the min_deadline heap. This leaves the heap in an inconsistent state where internal nodes have outdated deadline values.

## Fix Summary

Add a call to `min_deadline_cb_propagate(&se->run_node, NULL)` after updating the deadline in `reweight_entity()`. This function propagates the updated deadline value up the heap to restore invariants and prevent heap corruption.

## Triggering Conditions

The bug occurs when the EEVDF scheduler reweights an entity (task/task group) that is currently on a runqueue. Specifically:
- A task must be enqueued (`se->on_rq == true`) in the min_deadline heap
- The task undergoes weight changes via cgroup operations (e.g., CPU share adjustments, moving between cgroups)
- `reweight_entity()` scales the task's deadline but skips heap propagation for performance
- Subsequent scheduling decisions use corrupted heap data where parent nodes have stale minimum deadline values
- This manifests as "EEVDF scheduling fail, picking leftmost" when the scheduler's deadline-based selection fails

## Reproduce Strategy (kSTEP)

Use 3+ CPUs with cgroup hierarchy and multiple tasks. In `setup()`: create two cgroups with different CPU shares using `kstep_cgroup_create()` and `kstep_cgroup_set_weight()`. Create 3-4 tasks with `kstep_task_create()` and distribute across cgroups with `kstep_cgroup_add_task()`. In `run()`: wake all tasks with `kstep_task_wakeup()`, run for several ticks with `kstep_tick_repeat()` to populate the min_deadline heap, then trigger reweighting by changing cgroup shares with `kstep_cgroup_set_weight()`. Use `on_tick_begin()` callback to validate heap integrity by checking if min_deadline values increase up the heap. The bug manifests as heap invariant violations and eventual "picking leftmost" fallbacks in scheduler decisions.
