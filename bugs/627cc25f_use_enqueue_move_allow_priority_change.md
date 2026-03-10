# sched/deadline: Use ENQUEUE_MOVE to allow priority change

- **Commit:** 627cc25f84466d557d86e5dc67b43a4eea604c80
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline (SCHED_DEADLINE)

## Bug Description

After commit 6455ad5346c9 ("sched: Move sched_class::prio_changed() into the change pattern"), deadline tasks trigger balance callback warnings due to incorrect priority handling. The DEQUEUE_SAVE+ENQUEUE_RESTORE pattern does not preserve deadline priority, causing unexpected balance passes to be triggered during task enqueue operations when they should not be.

## Root Cause

The enqueue_dl_entity() function used ENQUEUE_RESTORE flag to check when to call setup_new_dl_entity(). However, ENQUEUE_RESTORE does not properly preserve deadline priority information, while the preceding changes in the prio_changed() pattern expect consistent priority state. This mismatch causes the scheduler balance callbacks to execute unexpectedly, generating warnings.

## Fix Summary

Change the flag check from ENQUEUE_RESTORE to ENQUEUE_MOVE in enqueue_dl_entity(). The ENQUEUE_MOVE flag is designed for priority changes and is set whenever a task becomes new to deadline scheduling, ensuring both proper priority preservation and that balance callbacks are correctly triggered according to the new prio_changed() pattern.

## Triggering Conditions

The bug requires deadline tasks that undergo DEQUEUE_SAVE+ENQUEUE_RESTORE operations, which occur during priority changes, cgroup moves, or CPU affinity updates. Specifically, tasks new to deadline scheduling or with expired deadlines that require setup_new_dl_entity() are affected. The ENQUEUE_RESTORE flag fails to preserve deadline priority information, causing the scheduler to trigger unexpected balance callbacks and generate warnings in the new prio_changed() pattern from commit 6455ad5346c9.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved). In setup(), create multiple deadline tasks with kstep_task_create() and configure them with deadline parameters using task priority APIs. In run(), trigger DEQUEUE_SAVE+ENQUEUE_RESTORE patterns by moving tasks between cgroups using kstep_cgroup_create(), kstep_cgroup_add_task(), or changing task priorities. Use on_sched_softirq_begin/end callbacks to monitor unexpected balance invocations. Check for balance callback warnings in kernel logs and verify that setup_new_dl_entity() is called with incorrect flags (ENQUEUE_RESTORE vs ENQUEUE_MOVE). Log deadline task state during enqueue operations to detect when deadline priority is not preserved across dequeue/enqueue cycles.
