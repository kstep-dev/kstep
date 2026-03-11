# rq-qos: fix missed wake-ups in rq_qos_throttle try two

- **Commit:** 11c7aa0ddea8611007768d3e6b58d45dc60a19e1
- **Affected file(s):** kernel/sched/wait.c
- **Subsystem:** core

## Bug Description

A deadlock occurs when multiple waiters (waiter1, waiter2) enter the wait queue nearly simultaneously while a waker is releasing resources. Both waiters call `prepare_to_wait_exclusive()`, observe that the queue has other sleepers, and decide to sleep without rechecking the waiting condition. By the time they sleep, the waker has already released resources but found no sleepers to wake, resulting in missed wake-ups and a deadlock with both processes hung indefinitely.

## Root Cause

The previous fix introduced a check using `!wq_has_single_sleeper()` to block waiters when sleepers already exist. However, this check is racy: multiple waiters can observe the same sleeper count during the time window between checking the condition and actually entering the sleep state. Once both waiters are asleep, no one rechecks the waiting condition, causing the waker to miss both processes.

## Fix Summary

The fix changes `prepare_to_wait_exclusive()` to return a boolean indicating whether the wait queue was empty before adding the current waiter. This guarantees that the first waiter entering the queue will always recheck the waiting condition before sleeping, ensuring forward progress. Subsequent waiters can use the return value to determine their behavior.

## Triggering Conditions

The bug requires a precise race in the block I/O throttling layer (`rq_qos_wait()`). Multiple processes must simultaneously attempt to acquire inflight I/O slots when the limit is exceeded, causing `acquire_inflight_cb()` to fail for all waiters. A waker thread must complete I/O operations (decreasing the inflight count) after the acquisition failures but before the waiters call `io_schedule()`. Both waiters observe existing sleepers via `!wq_has_single_sleeper()` and decide to sleep without rechecking the acquisition condition. The timing window is critical - the waker finds no sleepers to wake because both waiters haven't entered sleep yet, but by the time they do sleep, resources are available but no one will recheck.

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved). Create a cgroup with strict I/O bandwidth limits using `kstep_cgroup_create()` and `kstep_cgroup_write()` to set low throttling thresholds. Pin two I/O-heavy tasks to CPUs 1-2 using `kstep_task_pin()` and `kstep_task_create()`, then trigger simultaneous I/O bursts via `kstep_task_wakeup()` to hit the throttling limit. Use a third task on CPU 3 as the "completer" that finishes I/O operations with precise timing via `kstep_tick()` calls. Monitor via `on_tick_begin()` callback to log task states and detect when both waiters are stuck in `rq_qos_wait()` without being woken. The deadlock manifests as both I/O tasks remaining in UNINTERRUPTIBLE sleep indefinitely despite available I/O bandwidth, detectable through task state inspection and lack of I/O progress.
