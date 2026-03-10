# sched/core: Fix forceidle balancing

- **Commit:** 5b6547ed97f4f5dfc23f8e3970af6d11d7b7ed7e
- **Affected file(s):** kernel/sched/core.c, kernel/sched/idle.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

The forceidle balancer was being queued every time the idle task was selected via `set_next_task_idle()`, which is called not only from the main scheduling path but also from `rt_mutex_setprio()`'s change pattern. This caused the forceidle balancer to be queued from an unexpected context, violating locking invariants and causing crashes in ChromeOS. The balancer should only be queued when the idle task is selected through the main `pick_next_task()` path.

## Root Cause

The `queue_core_balance()` call was placed in `set_next_task_idle()`, which is used in multiple contexts beyond the main scheduling path. When `rt_mutex_setprio()` invokes `set_next_task()` as part of its task property change pattern, it unexpectedly triggers `queue_core_balance()` outside the `__schedule()` rq->lock context, violating the invariant that balance callbacks should only be queued under the scheduler's lock.

## Fix Summary

The fix moves `queue_core_balance()` from `set_next_task_idle()` to `pick_next_task()`, ensuring the forceidle balancer is only queued when the idle task is actually selected as the next task through the main scheduling path. This restricts forceidle balancer queueing to the proper `__schedule()` rq->lock context, preventing out-of-band callback issues.

## Triggering Conditions

This bug requires a core scheduling enabled system with forceidle balancing active. The core scheduler must have a `core_forceidle_count > 0`, indicating sibling CPUs are being forced idle due to security constraints. The bug is triggered when `rt_mutex_setprio()` or similar priority-changing operations invoke the `set_next_task()` pattern on a task that results in selecting the idle task. This causes `set_next_task_idle()` to be called outside the main `__schedule()` path, queuing the forceidle balancer from an improper locking context. The timing race occurs when the balance callback is invoked without the proper rq->lock held during `__schedule()`, violating the scheduler's locking invariants.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 1+ for the test). Create a core scheduling scenario with forced idling using `kstep_cgroup_create()` to set up different security domains and `kstep_cgroup_add_task()` to assign tasks to separate groups. Use `kstep_task_create()` and `kstep_task_wakeup()` to create tasks in different security contexts that will trigger forced idling. Set up a high-priority RT task with `kstep_task_fifo()` and manipulate its priority using `kstep_task_set_prio()` to trigger the `rt_mutex_setprio()` path. Monitor the forceidle state via `on_tick_begin()` callbacks and use `TRACE_INFO()` to log core scheduling decisions. The bug manifests as unexpected balance callback scheduling detected through kernel warnings or crashes when the forceidle balancer is queued from the wrong context during priority changes.
