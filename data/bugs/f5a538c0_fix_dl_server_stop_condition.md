# sched/deadline: Fix dl_server stop condition

- **Commit:** f5a538c07df26f5c601e41f7b9c7ade3e1e75803
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

The dl_server fails to stop when expected. Idle time and fair runtime are treated identically when accounting towards dl_server runtime, causing both to push the activation forward even when the server should be stopping. This prevents the dl_server from properly terminating when only idle time is consuming its remaining runtime in the zero-laxity wait state.

## Root Cause

When dl_server is in the deferred state (dl_defer), both idle and non-idle time advance the deadline by calling replenish_dl_new_period(), which pushes the activation forward indefinitely. There is no distinction between whether runtime was consumed by actual fair tasks or by system idle time, so idle periods prevent the dl_server_timer from firing and properly stopping the server.

## Fix Summary

Introduces a new flag `dl_defer_idle` to distinguish idle time from fair runtime. Once idle time has pushed the activation forward, this flag is set and idle time can only consume existing runtime without pushing the deadline further. Non-idle work clears the flag immediately. This allows dl_server_timer to fire and stop the server when only idle time remains.

## Triggering Conditions

The bug occurs in the deadline scheduler's dl_server (fair server) subsystem when:
- A dl_server is in deferred state (`dl_defer=1`) and becomes throttled (`dl_throttled=1`) 
- The server has exceeded its runtime quota (`dl_runtime_exceeded()` returns true)
- The CPU becomes idle with no fair tasks to run, triggering `dl_server_update_idle()`
- Idle time accounting calls `update_curr_dl_se()` which incorrectly pushes the activation forward via `replenish_dl_new_period()`
- This creates an infinite loop where idle time prevents `dl_server_timer()` from firing and properly stopping the server
- The server continuously renews its period instead of terminating when only idle time remains

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). Setup a CFS runqueue with dl_server and trigger the stop condition:
- In `setup()`: Create 2 CFS tasks using `kstep_task_create()` and `kstep_task_cfs()`, pin to CPU 1 with `kstep_task_pin()`
- In `run()`: Wake both tasks with `kstep_task_wakeup()`, run several ticks with `kstep_tick_repeat()` to activate dl_server
- Pause both tasks with `kstep_task_pause()` to drain CFS runqueue, leaving CPU 1 idle but dl_server still active
- Continue ticking with `kstep_tick_repeat()` to trigger idle time accounting in `dl_server_update_idle()`
- Use `on_tick_begin()` callback to monitor dl_server state: check `dl_se->dl_defer`, `dl_se->dl_throttled`, and `dl_se->dl_server_active` flags
- Bug is triggered when server remains active (`dl_server_active=1`) despite having no CFS tasks, with deadline continuously advancing
- Fixed kernel should set `dl_defer_idle=1` and eventually call `dl_server_stop()` to terminate the server
