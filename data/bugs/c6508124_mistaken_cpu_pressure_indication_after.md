# sched/psi: Fix mistaken CPU pressure indication after corrupted task state bug

- **Commit:** c6508124193d42bbc3224571eb75bfa4c1821fbb
- **Affected file(s):** kernel/sched/core.c, kernel/sched/stats.h
- **Subsystem:** PSI (Pressure Stall Information), core scheduling

## Bug Description

When sched_delayed tasks are migrated between runqueues by the load balancer while PSI considers them asleep, PSI misinterprets the migration requeue followed by a wakeup as a double queue operation. This causes an "inconsistent task state" kernel warning and, more critically, leads to permanent incorrect CPU pressure indication, corrupting PSI metrics that are used for workload and machine health monitoring.

## Root Cause

The delayed-dequeue feature allows tasks to remain queued even after blocking, but PSI's migration and enqueue logic did not account for this special state. PSI's dequeue handler could not distinguish between a voluntary sleep and a CPU migration for delayed tasks, and the psi_enqueue/dequeue parameter semantics were inverted—passing (wakeup=true) for migrations prevented proper handling of sleeping delayed tasks being migrated, leading to state corruption.

## Fix Summary

The fix reorders psi_enqueue() to be called after enqueue_task(), allowing PSI to observe the cleared sched_delayed flag and distinguish migrations from wakeups. It refactors psi_enqueue/dequeue logic to use a migrate parameter (instead of wakeup/sleep) and adds explicit handling to transfer sleeping states when delayed-dequeue tasks are migrated, while properly defaulting to wakeup handling for regular task transitions.

## Triggering Conditions

The bug requires tasks in sched_delayed state being migrated by the load balancer while PSI tracks them as asleep. Specifically: (1) Tasks must block and enter delayed-dequeue state where they remain queued despite blocking, (2) Load balancer must migrate these delayed tasks between runqueues while PSI considers them asleep, (3) PSI must be enabled and monitoring task state transitions, (4) The sequence migration-requeue followed by wakeup triggers PSI's double-queue detection, causing "inconsistent task state" warnings and permanent CPU pressure indication corruption.

## Reproduce Strategy (kSTEP)

Use 3+ CPUs (CPU 0 reserved for driver). In setup(), enable PSI via sysctl and create CFS tasks with different nice values to encourage load balancing. In run(), use kstep_task_wakeup() to start tasks on different CPUs, then kstep_task_pause() to force delayed-dequeue state on some tasks. Use kstep_tick_repeat() to create CPU load imbalance triggering migration of delayed tasks via load balancer. Monitor with on_tick_begin() callback to check PSI state and task migrations. Use kstep_task_wakeup() on migrated delayed tasks to trigger the double-queue bug. Detect bug via kernel log warnings ("inconsistent task state") and PSI metric corruption by checking /proc/pressure/cpu values before/after reproduction.
