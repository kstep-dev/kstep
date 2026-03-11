# Fix dl_server behaviour

- **Commit:** a3a70caf7906708bf9bbc80018752a6b36543808
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/sched.h
- **Subsystem:** Deadline

## Bug Description

The dl_server would yield when `server_pick_task()` returned NULL (indicating no runnable tasks), pushing future job invocations out by a whole period (1 second). This caused fair workload tasks that frequently go idle to experience severe delays, running only once per second instead of at expected rates, resulting in unexpectedly slow and poor scheduling behavior.

## Root Cause

The `dl_server_stopped()` function implemented a two-call mechanism where the first NULL return would yield the server (setting `dl_server_idle = 1`), and only on the second call would it actually stop. This was introduced in a previous commit to be "less aggressive," but had the unintended consequence of deferring the server to the next period whenever the server had no tasks, even if more tasks would wake up shortly.

## Fix Summary

Instead of yielding when no tasks are available, the server now immediately stops itself when `server_pick_task()` returns NULL. The server will be restarted on the next task wakeup, allowing fair tasks to resume promptly. The naturally-throttled timer period prevents excessive start/stop cycling, eliminating the need for the idle deferral logic.

## Triggering Conditions

The bug triggers when a dl_server is active and experiences the following sequence:
- Fair (CFS) tasks are present on the runqueue, causing the dl_server to activate
- High priority FIFO tasks starve the fair tasks, making them unable to run
- Fair tasks frequently go idle (blocked/sleeping), causing `server_pick_task()` to return NULL
- On the first NULL return, the buggy `dl_server_stopped()` sets `dl_server_idle = 1` and yields
- This pushes the next dl_server job out by a full period (typically 1 second)
- Subsequent fair task wakeups must wait until the next period before getting CPU time
- The two-call mechanism in `dl_server_stopped()` creates artificial delays for workloads with bursty fair tasks

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create fair tasks that alternate between running and idle states while higher-priority FIFO tasks create contention:

1. Setup: Create fair tasks and FIFO tasks using `kstep_task_create()`, set FIFO scheduling with `kstep_task_fifo()`
2. Pin fair tasks to CPU 1 with `kstep_task_pin(tasks, 1, 1)` to concentrate dl_server activity
3. Start fair tasks with `kstep_task_wakeup()`, then run `kstep_tick_repeat(10)` to activate dl_server
4. Pause fair tasks using `kstep_task_pause()` to trigger `server_pick_task()` returning NULL
5. Start competing FIFO tasks to create starvation conditions
6. Use `kstep_tick_repeat(100)` to advance time and observe server yielding behavior
7. Wakeup fair tasks with `kstep_task_wakeup()` and measure response latency
8. Use `on_tick_begin` callback to log dl_server state transitions and task scheduling events
9. Bug detected when fair tasks experience ~1 second delays between wakeup and execution despite available CPU
10. Compare scheduling latency before/after fair task pause to detect the yield-induced period delay
