# sched/deadline: Always stop dl-server before changing parameters

- **Commit:** bb4700adc3abec34c0a38b64f66258e4e233fc16
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** Deadline

## Bug Description

A previous commit reduced dl-server overhead by delaying server disabling, but this creates a race condition where changing server parameters while the server is still active can corrupt per-runqueue running_bw tracking. When dl_server_apply_params() is called before deadline entities have been dequeued, the bandwidth accounting becomes inconsistent, potentially breaking scheduler invariants.

## Root Cause

The delay in disabling servers opens a window where dl_server_apply_params() can be invoked while the server is still running. This causes new parameters to be applied to a server with deadline entities still queued, bypassing the proper dequeue operation needed to update running_bw tracking correctly.

## Fix Summary

The fix unconditionally calls dl_server_stop() before applying new parameters, removing the conditional check on `rq->cfs.h_nr_queued`. This ensures deadline entities always go through an actual dequeue operation before parameters change, maintaining running_bw consistency.

## Triggering Conditions

This bug requires the deadline server (fair_server) to be active while concurrently modifying its parameters through the debug interface. The dl-server must have active CFS tasks queued (`rq->cfs.h_nr_queued > 0`) so that it stays enabled due to the delayed disabling optimization. During this window, a write to `/sys/kernel/debug/sched/dl_server/CPU/runtime` or `period` files can trigger `dl_server_apply_params()` before the server's deadline entities are properly dequeued. The race occurs specifically in `sched_fair_server_write()` where the conditional `if (rq->cfs.h_nr_queued)` check allowed parameter changes without ensuring proper bandwidth accounting cleanup.

## Reproduce Strategy (kSTEP)

Setup 2+ CPUs and create multiple CFS tasks to ensure `h_nr_queued > 0` on target CPU. Use `kstep_cgroup_create()` and `kstep_cgroup_add_task()` to manage fair server load. In `run()`, first populate a runqueue with CFS tasks using `kstep_task_create()` and `kstep_task_wakeup()`. Call `kstep_tick_repeat()` to establish active dl-server state. Then use `kstep_write()` to modify `/sys/kernel/debug/sched/dl_server/CPU1/runtime` with different values while tasks remain queued. Use `on_tick_begin()` callback to monitor `running_bw` values before/after parameter changes. Check for bandwidth accounting inconsistencies by comparing `rq->dl.running_bw` across parameter modification boundaries. Log discrepancies with `TRACE_INFO()` to detect the corruption of per-runqueue bandwidth tracking that indicates successful bug reproduction.
