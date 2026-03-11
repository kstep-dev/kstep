# sched/rt: Fix double enqueue caused by rt_effective_prio

- **Commit:** f558c2b834ec27e75d37b1c860c139e7b7c3a8e4
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** RT (Real-Time scheduler)

## Bug Description

Double enqueues in RT runqueues occur when threads are concurrently switched between RT and fair scheduling classes while being subject to priority inheritance. The bug manifests as a kernel BUG/WARNING in `enqueue_task_rt()` when attempting to add a task that is already on the RT runqueue list, triggered during `sched_setscheduler()` calls.

## Root Cause

`__sched_setscheduler()` calls `rt_effective_prio()` twice: once before dequeuing the task and again inside `__setscheduler()` to set the priority. If the priority of the pi_top_task (priority inheritance top task) changes concurrently between these two calls, they may return different results. For example, the first call might return the task's current RT priority while the second returns a fair priority. This causes the task to remain enqueued in the RT list (on_list not cleared) while being moved to the fair runqueue, leading to a double enqueue when later setscheduled back to RT.

## Fix Summary

The fix eliminates the double call to `rt_effective_prio()` by computing the effective priority once and reusing it throughout the setscheduler operation. It also refactors the priority setting logic into a dedicated `__setscheduler_prio()` helper function to ensure consistent priority class assignment across all code paths.

## Triggering Conditions

This bug requires concurrent priority inheritance and setscheduler operations. The triggering sequence involves:
- A task with RT priority that has a priority inheritance chain (pi_top_task)
- Concurrent modification of the pi_top_task's priority during `__sched_setscheduler()`
- The first `rt_effective_prio()` call returns an RT priority (task stays in RT runqueue)
- The second `rt_effective_prio()` call in `__setscheduler()` returns a fair priority
- Task gets moved to fair runqueue while still marked as on the RT list (`on_list` not cleared)
- Later setscheduler back to RT triggers double enqueue warning in `enqueue_task_rt()`

## Reproduce Strategy (kSTEP)

Use 3+ CPUs with multiple RT tasks creating priority inheritance chains:
- **Setup**: Create RT mutex holder task and multiple RT waiter tasks at different priorities
- **Trigger**: Use `kstep_task_set_prio()` to rapidly change scheduler policies between RT and CFS while PI is active
- **Detection**: Monitor for double enqueue warnings via `on_sched_softirq_end()` callback
- **Implementation**: Create task A (RT prio 10), task B (RT prio 5), task C (CFS), have A block on mutex held by B, then rapidly alternate B's policy between RT/CFS while A is PI-boosted
- **Observation**: Check RT runqueue list integrity and task `on_rq` state during setscheduler operations
- **Success criteria**: BUG/WARNING in `enqueue_task_rt()` with "list_add double add" message
