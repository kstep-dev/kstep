# sched: Fix proxy/current (push,pull)ability

- **Commit:** be39617e38e0b1939a6014d77ee6f14707d59b1b
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/rt.c
- **Subsystem:** core (proxy execution with RT and deadline)

## Bug Description

When proxy execution is enabled, a task that acts as a proxy (execution context) for a blocked donor task can be incorrectly selected for push/pull migration. Since the donor and proxy form an atomic pair that must be kept together for CPU placement, pushing or pulling the proxy would improperly carry along the blocked donor task. The scheduling class logic (RT and deadline) can add the proxy task to their pushable lists even though it should be ineligible for migration while paired with a blocked task.

## Root Cause

The proxy task remains on the runqueue (rq->curr) after the donor task is dequeued and blocked. The scheduling class enqueue/put_prev_task logic doesn't account for this proxy-donor pairing, causing them to incorrectly add the proxy task to the pushable_list based on standard migration criteria, unaware that the task is actually serving as an execution context for a blocked task.

## Fix Summary

The fix adds a `proxy_tag_curr()` function that performs a dequeue/enqueue cycle on the proxy task after it's set as rq->curr, allowing the scheduling class logic to properly exclude it from the pushable_list. Additionally, checks using `task_is_blocked()` are added in the RT and deadline scheduling classes to prevent blocked tasks from being added to their pushable lists.

## Triggering Conditions

The bug requires proxy execution to be enabled and specific task blocking scenarios:
- A higher priority RT/deadline task (RT42) blocks on a mutex owned by a lower priority task (RT1)
- RT1 becomes the proxy execution context for the blocked RT42, forming an atomic donor-proxy pair
- CPU load imbalance exists where RT1's CPU is overloaded while other CPUs are available
- The RT/deadline scheduling class enqueue/put_prev_task logic processes RT1 without recognizing its proxy status
- RT1 gets incorrectly added to the pushable_list, making it eligible for push migration despite being paired with blocked RT42

## Reproduce Strategy (kSTEP)

Create two CPUs (driver uses CPU0, test uses CPU1-2) with load imbalance. Use `kstep_task_create()` to create RT42 (high priority) and RT1 (lower priority) tasks. Configure RT42 with `kstep_task_fifo()` and higher priority via `kstep_task_set_prio()`. Pin RT42 to CPU1 and RT1 to CPU2 using `kstep_task_pin()`. Create a mutex scenario where RT42 blocks on a resource owned by RT1 through carefully timed `kstep_task_wakeup()` and `kstep_task_pause()` calls. Monitor with `on_sched_balance_selected()` callback to detect when RT1 appears on pushable_list. Use `kstep_tick_repeat()` to advance scheduler state and trigger load balancing. Check internal scheduler state via logging to confirm RT1 is incorrectly flagged as pushable while serving as proxy for blocked RT42. Success criteria: RT1 appears in pushable_list despite proxy-donor pairing.
