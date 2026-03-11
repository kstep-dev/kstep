# sched/core: Fix RQCF_ACT_SKIP leak

- **Commit:** 5ebde09d91707a4a9bec1e3d213e3c12ffde348f
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition causes a RQCF_ACT_SKIP leak warning when CPU0 performs clock updates during `__schedule()` while CPU1 attempts to lock the same runqueue via `rq_clock_start_loop_update()` during scheduler group destruction. The `RQCF_ACT_SKIP` flag, intended to skip clock updates, is set early in `__schedule()` but cleared very late (in `context_switch()` or the else branch), leaving a window where another CPU can observe the flag in an inconsistent state and trigger a warning.

## Root Cause

The flag clearing operations (`rq->clock_update_flags &= ~(RQCF_ACT_SKIP|RQCF_REQ_SKIP)`) occurred too late after the clock update in `__schedule()`, allowing a race window where concurrent code paths (like `__cfsb_csd_unthrottle()`) could lock the runqueue and observe the flag still set, violating the expected flag state invariant.

## Fix Summary

Move the flag clearing to immediately after `update_rq_clock()` in `__schedule()` and explicitly set `rq->clock_update_flags = RQCF_UPDATED` to establish a consistent state. This eliminates the race window by ensuring the clock update flags are finalized before any subsequent code can acquire the runqueue lock.

## Triggering Conditions

The race requires two CPUs executing concurrently in scheduler core paths. CPU0 must be in `__schedule()` during load balancing (`newidle_balance()`) where it sets `RQCF_ACT_SKIP`, calls `update_rq_clock()`, then unlocks the runqueue via `rq_unpin_lock()` in `newidle_balance()`. CPU1 must simultaneously execute CFS bandwidth destruction (`destroy_cfs_bandwidth()` -> `__cfsb_csd_unthrottle()`) that attempts to lock CPU0's runqueue. The timing window occurs between CPU0's clock update and flag clearing, where CPU1 can acquire the lock and observe the stale `RQCF_ACT_SKIP` flag in `rq_clock_start_loop_update()`, triggering the warning. Tasks must be configured to cause CPU0 to enter idle load balancing while CPU1 processes cgroup destruction.

## Reproduce Strategy (kSTEP)

Use 3+ CPUs (driver on CPU0, targets on CPU1-2). Create multiple CFS tasks and cgroups with `kstep_task_create()`, `kstep_cgroup_create()`, and `kstep_cgroup_add_task()`. Configure asymmetric load with tasks pinned via `kstep_task_pin()` to force `newidle_balance()` on CPU1. Use `kstep_tick_repeat()` to advance scheduler state. In `setup()`, create tasks in separate cgroups, then in `run()` trigger the race by: (1) pausing/waking tasks to force CPU1 into idle balancing, (2) simultaneously destroying cgroups via cgroup filesystem writes using `kstep_write()` to trigger `destroy_cfs_bandwidth()`. Use `on_sched_balance_begin()` and `on_tick_begin()` callbacks to log runqueue state and detect when the race window opens. Check for RQCF_ACT_SKIP warnings in kernel logs to confirm reproduction.
