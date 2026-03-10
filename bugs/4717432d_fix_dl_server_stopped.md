# sched/deadline: Fix dl_server_stopped()

- **Commit:** 4717432dfd99bbd015b6782adca216c6f9340038
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

The `dl_server_stopped()` function returns `false` when `dl_se->dl_server_active` is 0, but it should return `true` since a server with `dl_server_active == 0` is indeed stopped. This logical error causes the function to return the opposite of the expected semantics, leading to incorrect deadline server state tracking and triggering spurious "sched: DL replenish lagged too much" warnings during boot.

## Root Cause

The function was introduced in commit cccb45d7c429 with inverted return value logic. When the server is inactive (`dl_server_active == 0`), the function should immediately return `true` to indicate the server is stopped, but instead it returned `false`, causing callers to incorrectly interpret the server state.

## Fix Summary

Changed the return value from `false` to `true` in the early exit condition of `dl_server_stopped()` when `!dl_se->dl_server_active`. This corrects the semantic meaning of the function to properly indicate that a server with `dl_server_active == 0` is indeed stopped.

## Triggering Conditions

The bug is triggered when code calls `dl_server_stopped()` on a deadline server entity where `dl_server_active == 0`. This occurs during deadline server lifecycle management, particularly during server initialization, cleanup, or state transitions. The inverted return value causes callers to misinterpret inactive servers as active, potentially leading to incorrect scheduling decisions and spurious warnings like "sched: DL replenish lagged too much" during boot when the deadline infrastructure processes server state changes.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). In `setup()`, create multiple tasks with `kstep_task_create()` and configure some as deadline servers using the deadline scheduler class. In `run()`, manipulate server lifecycle by starting servers with `dl_server_start()`, then stopping them with `dl_server_stop()` which sets `dl_server_active = 0`. Add callback instrumentation in `on_tick_begin()` to monitor server states and call `dl_server_stopped()` on inactive servers. Log the return values to detect the inverted logic: the function should return `true` for stopped servers but will incorrectly return `false` in the buggy kernel, demonstrating the semantic inversion that causes state tracking errors.
