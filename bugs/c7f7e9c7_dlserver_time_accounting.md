# sched/dlserver: Fix dlserver time accounting

- **Commit:** c7f7e9c73178e0e342486fd31e7f363ef60e3f83
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** Deadline, Fair scheduling

## Bug Description

The dlserver time accounting was incorrect because `dl_server_update()` was being called in two places without properly verifying that the dlserver was active. This could result in incorrect time accounting for deadline server tasks, especially in cases where a fair task runs either on behalf of the dlserver or outside of it after the dlserver is deferred. The bug manifests when accounting logic doesn't distinguish between these two valid execution scenarios.

## Root Cause

The original implementation called `dl_server_update()` from two different locations: once in `update_curr_task()` (unconditionally) and again later in `update_curr()` with an incorrect condition check (`if (p->dl_server != &rq->fair_server)`). This condition didn't properly account for all cases where dlserver time should be tracked, and the dual calls could lead to either missing updates or incorrect state management when dlserver is active but not properly tracked.

## Fix Summary

The fix consolidates `dl_server_update()` into a single location in `update_curr()` and replaces the flawed condition check with `dl_server_active(&rq->fair_server)`, which properly determines whether the fair_server is actively running. This ensures that dlserver time is correctly accounted for in both scenarios: when the dlserver directly proxies a cfs task, and when a cfs task runs after the dlserver is deferred.

## Triggering Conditions

The bug occurs in the deadline server mechanism when fair tasks run in mixed dlserver/non-dlserver contexts. Specifically:
- A fair task must be associated with a dlserver (`p->dl_server` set)
- The dlserver becomes active but later gets deferred
- The fair task continues running after dlserver deferral
- The flawed condition `p->dl_server != &rq->fair_server` incorrectly skips dlserver time accounting
- This leads to inconsistent runtime tracking where dlserver time is either double-counted or missed entirely
- The bug manifests during `update_curr()` calls in the fair scheduler when tasks transition between dlserver-proxied and direct execution modes

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. Create a fair task that gets associated with the dlserver, force dlserver activation and deferral:
- In `setup()`: Create 2 fair tasks using `kstep_task_create()` and configure dlserver parameters
- In `run()`: Use `kstep_task_wakeup()` to activate both tasks, then `kstep_tick_repeat()` to let them execute
- Force dlserver activation by manipulating task weights/runtime to exceed dlserver bandwidth
- Use `kstep_task_pause()` on one task to trigger dlserver deferral conditions
- Continue execution with `kstep_tick_repeat()` to observe the remaining task running outside dlserver context
- Monitor dlserver runtime accounting via `on_tick_end()` callback, logging dlserver state and task execution time
- Compare actual vs expected dlserver runtime to detect double-counting or missed accounting
- Check for inconsistent `rq->fair_server` runtime values across scheduling events
