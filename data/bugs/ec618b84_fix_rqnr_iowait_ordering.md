# sched: Fix rq->nr_iowait ordering

- **Commit:** ec618b84f6e15281cc3660664d34cd0dd2f2579e
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition exists where `nr_iowait` can be decremented before it is incremented, resulting in incorrect IO-wait accounting. When a task is woken up via `try_to_wake_up()` (decrementing `nr_iowait`) before the previous CPU in `schedule()` has finished incrementing it (in the `in_iowait` check), the counter can briefly go negative or miss updates. This leads to inaccurate IO-wait statistics and dodgy scheduling metrics.

## Root Cause

The bug is caused by a loss of natural ordering introduced by an earlier optimization (commit c6e7bd7afaeb) that allowed `ttwu_queue_wakelist()` to be called before `p->on_cpu` is cleared to 0. The original code decremented `nr_iowait` early in `try_to_wake_up()` without proper synchronization with the increment side in `schedule()`. When a task transitions from blocked (in I/O wait) to runnable on a different CPU, there is no guarantee that the previous CPU's increment happens before the wakeup path's decrement.

## Fix Summary

The fix moves the `nr_iowait` decrement from its original early location in `try_to_wake_up()` to two strategic locations: into `ttwu_do_activate()` (for non-migrated or non-SMP cases) and after `select_task_rq()` when the task is actually being migrated to a different CPU. This delayed decrement ensures that the `on_cpu` flag has been properly cleared and ordering is maintained, preventing the race condition.

## Triggering Conditions

The race occurs when a task transitions from I/O wait to runnable state across different CPUs. Specifically:
- A task must be in I/O wait state (`task->in_iowait = 1`) and blocking on a syscall
- The task gets woken up via `try_to_wake_up()` on a different CPU than where it's currently scheduled
- The wakeup path (`ttwu()`) executes before the original CPU completes its `schedule()` transition
- In the race window, `ttwu()` decrements `nr_iowait` while the original CPU hasn't yet incremented it in `schedule()`
- Requires SMP system where `ttwu_queue_wakelist()` optimization allows early wakeup processing
- The timing must be precise: wakeup processing starts after `deactivate_task()` but before the `in_iowait` check in `schedule()`

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 1,2 since CPU 0 is reserved). Create timing-sensitive I/O wait transitions:
- **Setup**: Create 2 tasks, pin them to different CPUs (1 and 2)
- **I/O Wait Entry**: Use `kstep_task_pause()` to simulate I/O wait on task1 (CPU 1), ensure `in_iowait` flag is set
- **Race Trigger**: Use `kstep_task_wakeup()` from CPU 2 to wake task1 with precise timing via `kstep_tick()` control
- **Timing Control**: Use `kstep_tick_repeat(1)` to create narrow race windows between pause and wakeup operations
- **Detection**: Monitor `rq->nr_iowait` values via custom logging in `on_tick_begin()` callback
- **Validation**: Check for temporary negative `nr_iowait` values or missed increments/decrements
- **Stress Test**: Repeat the pause/wakeup cycle rapidly with `kstep_tick()` to increase race probability
- Use `kstep_sleep()` and `kstep_task_usleep()` to create realistic I/O wait scenarios
