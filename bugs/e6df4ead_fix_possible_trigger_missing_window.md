# psi: fix possible trigger missing in the window

- **Commit:** e6df4ead85d9da1b07dd40bd4c6d2182f3e210c4
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

When a threshold-breaching stall event occurs after a previous PSI event was generated and within the same rate-limiting window, the new breach is not reported because events are rate-limited to one per window. If no further stall activity occurs after this missed breach, the event is never generated even after the rate-limiting period expires. This results in lost notifications of threshold breaches to userspace monitors, breaking the contract that all breaches above a threshold will eventually generate events.

## Root Cause

The `update_triggers()` function only processes triggers when there is new stall activity detected (i.e., when `group->polling_total[t->state] != total[t->state]`). When a threshold is breached but rate-limited due to the window check (`if (now < t->last_event_time + t->win.size)`), the code continues without recording this breach. If subsequent polling finds no new stalls (the stall state is unchanged), `window_update()` is never called and the pending breach is never generated as an event.

## Fix Summary

The fix introduces a `pending_event` flag to record when a threshold has been breached, independent of whether the event was immediately rate-limited. The logic now generates events for both new stall activity and previously pending breaches, and resets the flag once the event is sent. This ensures threshold breaches are never lost and events are eventually generated once the rate-limiting window expires.

## Triggering Conditions

This bug occurs in the PSI subsystem when userspace monitors register pressure threshold triggers. The sequence requires: (1) A PSI trigger with a specific threshold and rate-limiting window size, (2) An initial pressure stall that exceeds the threshold, generating an event and starting the rate-limiting window, (3) A second pressure stall that exceeds the threshold within the rate-limiting window, causing the breach to be ignored due to rate-limiting, (4) No further pressure stall activity after the ignored breach, preventing `window_update()` from being called. The bug manifests when the rate-limiting window expires but the pending breach from step 3 is never reported to userspace, violating the contract that all threshold breaches generate events.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved). In `setup()`, create pressure triggers via PSI interfaces and configure multiple tasks for controlled stall generation. In `run()`, use `kstep_task_create()` to create competing tasks on limited CPUs, then `kstep_task_wakeup()` to trigger initial pressure stalls exceeding the threshold. Use `kstep_tick_repeat()` to advance time within the rate-limiting window, then trigger a second pressure stall burst with more `kstep_task_wakeup()` calls. Finally, let time advance past the rate-limiting window with `kstep_tick_repeat()` while keeping stall levels below threshold. Use callbacks `on_tick_begin()` to monitor PSI state and detect missing events by checking if the second threshold breach was never reported to userspace despite the window expiring. Log PSI pressure values and trigger event counts to verify the bug.
