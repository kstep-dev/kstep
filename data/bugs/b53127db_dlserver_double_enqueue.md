# sched/dlserver: Fix dlserver double enqueue

- **Commit:** b53127db1dbf7f1047cf35c10922d801dcd40324
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/sched.h
- **Subsystem:** Deadline

## Bug Description

The deadline server (dlserver) can be dequeued during a dlserver pick_task operation due to the delayed dequeue feature, but dlserver logic still thinks it is on the runqueue. This causes the dlserver throttling and replenish logic to become confused, resulting in double enqueue of the dlserver. Double enqueue corrupts scheduler state and leads to incorrect task scheduling decisions.

## Root Cause

Two separate issues cause this bug. First, the delayed dequeue feature can dequeue the dlserver during a pick operation initiated by the dlserver itself, but the code continues with time accounting (`update_curr_dl_se`) without knowing the dlserver has been dequeued. Second, a race condition exists where one CPU dequeues a task while another CPU enqueues it via try_to_wake_up with the runqueue lock released, causing both `dl_server_start` and `dl_server_update->enqueue_dl_entity` to execute, resulting in double enqueue. The lack of an explicit status flag representing dlserver's actual enqueue state is the root cause.

## Fix Summary

The fix introduces an explicit `dl_server_active` flag that tracks whether the dlserver is currently enqueued. This flag is set in `dl_server_start()` and cleared in `dl_server_stop()`. The code in `__pick_task_dl()` is modified to only update dlserver time accounting when the server is active, preventing spurious updates to dequeued servers and avoiding the conditions that trigger double enqueue.

## Triggering Conditions

This bug requires dlserver to be active with CFS tasks using deadline server bandwidth control. Two scenarios trigger the double enqueue: (1) delayed dequeue during dlserver pick_task where `pick_task_dl -> server_pick_task -> pick_task_fair -> pick_next_entity` with `sched_delayed` set causes `dequeue_entities -> dl_server_stop`, but `server_pick_task` continues with `update_curr_dl_se` on the dequeued server; (2) multi-CPU race where one CPU releases runqueue lock during `schedule() -> pick_next_task_fair() -> sched_balance_newidle()` while another CPU executes `try_to_wake_up() -> activate_task() -> dl_server_start()` followed by `wakeup_preempt() -> update_curr() -> dl_server_update() -> enqueue_dl_entity()`. Both scenarios require dlserver time accounting confusion where `dl_throttled` and `dl_yield` flags get set incorrectly.

## Reproduce Strategy (kSTEP)

Setup requires at least 2 CPUs (CPU 1,2) with CFS tasks that trigger dlserver activation. In `setup()`, create multiple CFS tasks using `kstep_task_create()` and configure deadline server via cgroup bandwidth controls with `kstep_cgroup_create()` and `kstep_cgroup_set_weight()`. In `run()`, use `kstep_task_pin()` to distribute tasks across CPUs, then alternate between `kstep_task_pause()` and `kstep_task_wakeup()` to trigger the race condition while calling `kstep_tick_repeat()` to advance scheduling decisions. Use `on_tick_begin()` callback to monitor dlserver state and detect double enqueue by checking if the same dlserver entity appears multiple times in deadline runqueue structures. Log dlserver enqueue/dequeue events and verify corruption by examining runqueue consistency after each scheduling operation.
