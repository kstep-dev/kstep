# Revert "sched/fair: Make sure to try to detach at least one movable task"

- **Commit:** 2feab2492deb2f14f9675dd6388e9e2bf669c27a
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Fair scheduler)

## Bug Description

The previous commit b0defa7ae03ec modified load balancing to ignore the task examination limit (env.loop_max) when all examined tasks were pinned. This caused detach_tasks() to perform a full O(n) scan of all tasks on a CPU when pinned tasks dominated the list. Since load balancing code executes with rq lock held in softirq context, examining thousands of tasks without yielding triggered hard lockups on systems with O(10k) pinned threads affined to a single CPU.

## Root Cause

The reverted patch removed the loop_max boundary check, allowing detach_tasks() to iterate through every task on the runqueue when encountering primarily pinned tasks. Holding rq->lock across such a long iteration in softirq context prevented the scheduler from yielding control, causing hard lockups before softirq processing could complete or be interrupted.

## Fix Summary

The fix reverts the problematic patch, restoring the loop_max limit check in detach_tasks(). This ensures the load balancer respects the iteration limit and yields control periodically via LBF_NEED_BREAK, preventing the O(n) iteration pathology and avoiding hard lockups.

## Triggering Conditions

The bug manifests in the CFS load balancing path, specifically in `detach_tasks()` during softirq processing. Required conditions:
- A source CPU with thousands of tasks (O(10k)) all pinned to that CPU via CPU affinity
- Load imbalance that triggers active load balancing from the overloaded CPU
- The `LBF_ALL_PINNED` flag gets set as the load balancer encounters only pinned tasks
- With the buggy code, `env->loop_max` limit is ignored when `LBF_ALL_PINNED` is set
- This causes full O(n) scan of the entire task list while holding `rq->lock` in softirq context
- The prolonged lock holding without yielding triggers hard lockup detection

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved for driver, CPU 1 overloaded, CPU 2+ targets for balancing):
- **Setup**: Create 1000+ tasks using `kstep_task_create()` and pin them all to CPU 1 with `kstep_task_pin(task, 1, 1)`
- **Load imbalance**: Keep other CPUs (2, 3) mostly idle to create significant load imbalance
- **Trigger balancing**: Use `kstep_tick_repeat()` to advance time and trigger periodic load balancing
- **Detection**: Monitor `on_sched_softirq_begin/end` callbacks - with the bug, softirq processing will hang
- **Observation**: Measure time spent in softirq context; excessive duration (>100ms) indicates the O(n) scan
- **Verification**: Check that load balancing attempts but fails to move tasks due to affinity constraints
- Log `env->loop` iterations vs `env->loop_max` to confirm limit bypass behavior
