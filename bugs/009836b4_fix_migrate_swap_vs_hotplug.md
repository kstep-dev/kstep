# sched/core: Fix migrate_swap() vs. hotplug

- **Commit:** 009836b4fa52f92cba33618e773b1094affa8cd2
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition occurs between migrate_swap() and CPU hotplug (sched_cpu_deactivate()). When migrate_swap() calls stop_two_cpus() on CPU0 while CPU1 is performing CPU hotdown, the stopper task can get stuck in a WAKING state and fail to become runnable. This allows subsequent balance_push() operations to execute before the previous one completes, triggering a double list add error and causing the balance_push mechanism to malfunction.

## Root Cause

The bug is caused by the interaction of two wakeup mechanisms (wake_q and ttwu_wakelist) that can defer critical wakeups of stopper threads. When a stopper task is added to the remote CPU's wakelist during a CPU hotplug event, the wakeup is delayed instead of being processed immediately. This delays the stopper from becoming runnable, allowing schedule() to be called again on the affected CPU and incorrectly execute another balance_push() before the first one completes.

## Fix Summary

The fix adds a check in ttwu_queue_cond() to ensure stopper threads (with sched_class == &stop_sched_class) are never queued or delayed through the wakelist mechanism, forcing their wakeups to be handled directly and immediately. This prevents the race condition by guaranteeing stopper threads become runnable promptly during hotplug operations.

## Triggering Conditions

The race requires precise timing between migrate_swap() and CPU hotplug operations. CPU0 must call stop_two_cpus() for migrate_swap() while CPU1 simultaneously performs sched_cpu_deactivate() during CPU hotdown. The critical condition is that the migrate/1 stopper thread wakeup gets deferred through ttwu_wakelist instead of executing immediately. This happens when ttwu_queue_cond() routes the wakeup to the remote wakelist due to the target CPU being different from the current CPU. The delayed stopper wakeup allows CPU1 to call schedule() again, triggering a second balance_push() before the first completes, leading to double list addition errors in the cpu_stop work queue.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver, use CPUs 1-2). In setup(), create tasks to trigger migrate_swap() and simulate CPU hotplug conditions. Use kstep_task_create() and kstep_task_pin() to create pinned tasks on CPU1 and CPU2. In run(), orchestrate the race by having CPU1 start balance_push operations while triggering migrate_swap() from CPU0. Use kstep_tick_repeat() to advance the system state and create timing windows. Monitor stopper thread states and balance_push work queue operations through on_tick_begin() callbacks that log CPU stop work queue states and stopper thread status. Detect the bug by checking for duplicate work items in cpu_stop work queues or balance_push error messages in kernel logs, indicating the double list add condition was triggered.
