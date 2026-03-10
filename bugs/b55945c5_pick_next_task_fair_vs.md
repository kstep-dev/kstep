# sched: Fix pick_next_task_fair() vs try_to_wake_up() race

- **Commit:** b55945c500c5723992504aa03b362fab416863a6
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS

## Bug Description

A race condition exists between `pick_next_task_fair()` and `try_to_wake_up()` where both code paths concurrently write to the same task's `p->on_rq` field, causing KCSAN to report assertion violations in `__block_task()`. This occurs when the store to `p->on_rq = 0` in `__block_task()` becomes visible to `try_to_wake_up()`'s load of `p->on_rq`, causing ttwu to incorrectly assume the task is not queued and proceed to migrate it, even though the original dequeue still holds `rq->__lock` on a different runqueue.

## Root Cause

The issue is a loss of mutual exclusion: `__block_task()` stores `p->on_rq = 0` without proper ordering guarantees, allowing `try_to_wake_up()` to observe this write before the scheduler releases its lock. Since both paths can hold different `rq->__lock` instances (for different CPUs), they can execute concurrently despite holding locks, violating the ASSERT_EXCLUSIVE_WRITER assertion that expects single-writer access to `p->on_rq`.

## Fix Summary

The fix uses `smp_store_release()` with proper memory ordering instead of `WRITE_ONCE()` to ensure the write becomes visible only after prior scheduler operations complete. Additionally, code that references the task pointer is moved before the `__block_task()` call, with callers explicitly warned not to reference the task after this function, ensuring the scheduler relinquishes all access before the write can be observed by concurrent wake-up paths.

## Triggering Conditions

This race occurs in the delayed dequeue code path within `pick_next_entity()` when:
- A task has `se->sched_delayed` set and is selected by `pick_eevdf()`
- `dequeue_entities()` is called with `DEQUEUE_SLEEP | DEQUEUE_DELAYED`, which invokes `__block_task()` to set `p->on_rq = 0`
- Concurrently, `try_to_wake_up()` runs on a different CPU, observing the `p->on_rq = 0` write before the scheduler releases its lock
- The timing window exists between `__block_task()`'s store and the lock release, allowing ttwu to assume the task is not queued
- Both paths hold different `rq->__lock` instances (for different CPUs), enabling concurrent execution
- KCSAN detects the race when both paths write to `p->on_rq` simultaneously

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved). In `setup()`:
- Create multiple tasks with `kstep_task_create()`
- Pin tasks to different CPUs to force cross-CPU operations
- Configure tasks to trigger delayed dequeue behavior (e.g., sleep/wake patterns)

In `run()`:
- Use `kstep_task_wakeup()` and `kstep_task_pause()` to create delayed dequeue conditions
- Trigger concurrent operations: one task becoming sched_delayed on CPU 1, while attempting `kstep_task_wakeup()` from CPU 2
- Use `kstep_tick()` to advance scheduler state and create the race window
- Monitor with `on_tick_begin` callback to log task states and `p->on_rq` values

Detection: Look for KCSAN warnings about `ASSERT_EXCLUSIVE_WRITER(p->on_rq)` violations in kernel logs, or inconsistent task migration behavior between buggy and fixed kernels.
