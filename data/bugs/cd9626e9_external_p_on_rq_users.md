# sched/fair: Fix external p->on_rq users

- **Commit:** cd9626e9ebc77edec33023fe95dab4b04ffc819d
- **Affected file(s):** include/linux/sched.h, kernel/events/core.c, kernel/freezer.c, kernel/rcu/tasks.h, kernel/sched/core.c
- **Subsystem:** CFS (Fair scheduler)

## Bug Description

KVM's preemption notifiers were mis-classifying preemption vs blocking after the delayed dequeue feature was introduced. The issue manifests when code checks `p->on_rq` to determine if a task is runnable, but the delayed dequeue feature allows tasks to remain on the runqueue with `sched_delayed=true` even though they will not execute and should be considered blocked. This causes external subsystems (perf, freezer, RCU) to make incorrect decisions about task state.

## Root Cause

Commit 152e11f6df29 ("sched/fair: Implement delayed dequeue") introduced a new scheduling state where tasks can have `p->on_rq=1` but `p->se.sched_delayed=true`, meaning they are on the runqueue but will be dequeued on their next pick and will not actually run. Code external to the scheduler that relied on `p->on_rq` as a reliable indicator of runnability now makes incorrect state classifications, causing preemption to be confused with blocking in KVM preemption notifiers and similar issues in other subsystems.

## Fix Summary

The fix introduces a `task_is_runnable()` helper that correctly identifies truly runnable tasks by checking both `p->on_rq && !p->se.sched_delayed`. It then audits and updates all external users of `p->on_rq` state (perf events, task freezing, RCU, task_call_func documentation) to use this helper instead of checking `on_rq` directly, ensuring consistent and correct task state classification across the kernel.

## Triggering Conditions

The bug triggers when the CFS delayed dequeue feature marks tasks with `sched_delayed=true` while keeping them `on_rq=1`. This occurs when:
- A task blocks (sleeps, waits, pauses) but remains on the runqueue for performance reasons  
- External code checks `p->on_rq` to determine task state, incorrectly classifying delayed tasks as runnable
- The delayed task gets picked by the scheduler but is immediately dequeued since it's actually blocked
- Race window exists between task blocking and actual dequeue, where `on_rq=1` but task won't run
- Affects KVM preemption notifiers, perf events, task freezing, and RCU which rely on `on_rq` state
- Most easily triggered with workloads that have frequent block/wakeup patterns and external task monitoring

## Reproduce Strategy (kSTEP)

Need at least 2 CPUs. In `setup()`, create tasks and enable any external monitoring that checks `on_rq`:
- Create tasks with `kstep_task_create()`, pin to different CPUs with `kstep_task_pin()`  
- Set up perf monitoring or freezer state that depends on `p->on_rq` checks
- Use `on_tick_begin()` callback to monitor task states and log `p->on_rq` vs `p->se.sched_delayed`

In `run()`, create the delayed dequeue scenario:
- Wake tasks with `kstep_task_wakeup()`, let them run with `kstep_tick_repeat(10)`
- Pause tasks with `kstep_task_pause()` to trigger delayed dequeue state  
- Immediately check task state before next tick - should see `on_rq=1` but `sched_delayed=true`
- Log external subsystem decisions (perf, freezer) and compare against actual task behavior
- Success: external code misclassifies delayed tasks as runnable when they're actually blocked
