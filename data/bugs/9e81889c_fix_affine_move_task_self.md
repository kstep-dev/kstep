# sched: Fix affine_move_task() self-concurrency

- **Commit:** 9e81889c7648d48dd5fe13f41cbc99f3c362484a
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When two concurrent `sched_setaffinity()` calls target the same task with different CPU affinities, both calls may read the same `p->migration_pending` pointer and issue `stop_one_cpu_nowait()` on it. This causes the same pending structure to be added to the stopper work queue multiple times, resulting in stopper list corruption.

## Root Cause

The `affine_move_task()` function lacked synchronization to prevent a second `sched_setaffinity()` call from concurrently reading and reusing the same `p->migration_pending` while a stopper is already enqueued for it. Without tracking whether a stopper is actually in progress (just checking the pointer is insufficient), multiple calls can race to enqueue the same pending structure multiple times.

## Fix Summary

A new `stop_pending` boolean field is added to `struct set_affinity_pending` to track whether a stopper is currently in progress. Before issuing `stop_one_cpu_nowait()`, the code checks and sets `stop_pending = true`, and only one concurrent caller will actually queue the stopper. The field is cleared when the stopper completes, preventing duplicate entries in the stopper queue.

## Triggering Conditions

The bug requires two concurrent `sched_setaffinity()` calls targeting the same task with different CPU affinity masks. Both calls must reach `affine_move_task()` simultaneously and read the same non-NULL `p->migration_pending` pointer before either can set the `stop_pending` flag. This race occurs when:
- A task already has a pending affinity change (`migration_pending` is set)
- Two threads simultaneously call `sched_setaffinity()` on the same task
- Both threads observe the same `pending` structure and attempt to enqueue it via `stop_one_cpu_nowait()`
- Without the `stop_pending` synchronization, both calls add the same work item to the stopper queue
- This causes stopper list corruption when the same `cpu_stop_work` is linked multiple times

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 1+ available for tasks). Create a target task and two kthreads that concurrently change its CPU affinity:
- In `setup()`: Create one target task and two kthread workers using `kstep_task_create()` and `kstep_kthread_create()`
- Pin the target task to CPU 1 initially with `kstep_task_pin(target, 1, 1)`
- In `run()`: Wake the target task, then simultaneously wake both kthreads
- Each kthread calls `kstep_task_pin()` on the target with different CPU sets (e.g., CPU 1 vs CPU 2)
- Use `kstep_tick()` to create timing windows where both kthreads reach `affine_move_task()` concurrently
- Monitor stopper queue corruption through kernel logs or by observing scheduling anomalies
- Detection: Look for kernel warnings about list corruption or use debugging to verify duplicate stopper work entries
