# Fix wakeup_preempt's next_class tracking

- **Commit:** 5324953c06bd929c135d9e04be391ee2c11b5a19
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c, kernel/sched/ext.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

During newidle balance operations, the rq->next_class could be incorrectly elevated from idle_sched_class to fair_sched_class. This caused subsequent wakeup_preempt() calls to fail the sched_class_above() check and miss issuing resched_curr(), leading to missed preemptions. Additionally, when schedule_idle() was called, rq->next_class might not be properly reset to idle_sched_class, causing the same preemption issue.

## Root Cause

The code was directly assigning sched_class pointers to rq->next_class without enforcing the invariant that next_class should only be lowered (pointing to less critical classes), never raised. This invariant was violated in two scenarios: (1) during balance_one() in the newidle path, and (2) when returning to idle in schedule_idle(). Without proper guards, these assignments could increase next_class to a higher-priority class, breaking subsequent wakeup preemption logic.

## Fix Summary

The fix introduces two helper functions: `rq_modified_begin()` which only lowers next_class (never raises it), and `rq_modified_above()` which checks if next_class has been modified above a given class. These helpers enforce the invariant that next_class can only decrease in priority. Additionally, `rq->next_class = &idle_sched_class` is explicitly set in schedule_idle() when no tasks are running, ensuring the correct initial state.

## Triggering Conditions

The bug is triggered when the idle thread performs newidle balance and hits the balance_one() code path in sched_balance_newidle(). Specifically:
- CPU must be in idle state (`rq->nr_running == 0`) to enter newidle balancing
- Load balancing logic must execute and modify `rq->next_class` from `&idle_sched_class` to `&fair_sched_class` 
- A subsequent task wakeup must call wakeup_preempt(), which fails the sched_class_above() check due to elevated next_class
- The bug manifests as missed preemptions where resched_curr() is not called when it should be
- Race window exists between balance_one() elevating next_class and the next wakeup_preempt() call
- Also triggered in schedule_idle() path when next_class is not properly reset to idle_sched_class after task selection

## Reproduce Strategy (kSTEP)

Create a scenario that forces newidle balance with subsequent task wakeups. Requires minimum 3 CPUs (CPU 0 reserved for driver):
- **Setup**: Use `kstep_topo_init()` and `kstep_topo_apply()` to configure multi-CPU topology. Create 2-3 tasks with `kstep_task_create()`
- **Trigger newidle balance**: Pin tasks to different CPUs with `kstep_task_pin()`, then make CPU 1 idle by pausing its task with `kstep_task_pause()`. Use `kstep_tick_repeat()` to trigger balance_one() from idle thread
- **Force wakeup preempt**: After newidle balance, use `kstep_task_wakeup()` to wake a high-priority task that should preempt current
- **Detection**: Use `on_tick_begin()` callback to log rq->next_class values and check for incorrect elevation. Monitor for missed resched_curr() calls using scheduler debug output with `kstep_print_sched_debug()`
- **Validation**: Compare preemption behavior before and after the wakeup - missed preemptions indicate the bug is triggered
