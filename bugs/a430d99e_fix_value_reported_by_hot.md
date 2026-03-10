# Fix value reported by hot tasks pulled in /proc/schedstat

- **Commit:** a430d99e349026d53e2557b7b22bd2ebd61fe12a
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The `lb_hot_gained` statistic in `/proc/schedstat` incorrectly counts hot tasks as pulled during load balancing even when they are not actually migrated. The load balancer's `can_migrate_task()` function increments this counter when deciding a hot task is eligible for migration, but the subsequent detach logic may still reject the migration due to other constraints, causing the stat to report inflated numbers.

## Root Cause

The statistics were being incremented in `can_migrate_task()` at the point where the decision was made to allow migration of a hot task, but the actual detachment logic in `detach_tasks()` could still skip the task without migrating it. This resulted in a mismatch between reported stats and actual task migrations, violating the intended semantics of `lb_hot_gained`.

## Fix Summary

The fix defers the statistics update from `can_migrate_task()` to the actual detachment point in `detach_task()`. A flag (`sched_task_hot`) is introduced to mark hot tasks that were approved for migration, and the stats are only incremented when the task is actually detached. This ensures `lb_hot_gained` accurately reflects tasks that were truly pulled, not just candidates that were considered.

## Triggering Conditions

This bug occurs during CFS load balancing when:
- Load imbalance exists between CPUs triggering `detach_tasks()` 
- Hot cache tasks (recently executed) are eligible for migration but later rejected
- The task passes `can_migrate_task()` checks (cpu affinity, not running, etc.)
- Task fails cache locality checks (`task_hot()` returns 1) but migration is forced due to `nr_balance_failed > cache_nice_tries`
- `detach_tasks()` decides not to migrate the task despite it being marked hot-eligible (e.g., load threshold reached)
- Results in `lb_hot_gained` being incremented without actual task migration

## Reproduce Strategy (kSTEP)

Create load imbalance with hot tasks that get approved but not migrated:
- Use 3+ CPUs (CPU 0 reserved for driver)
- Setup: Create 4-6 tasks, pin heavily to CPU 1, leave CPU 2-3 lighter
- Run tasks briefly on CPU 1 to make them cache hot: `kstep_tick_repeat(5)`
- Use `kstep_task_pause()` then `kstep_task_pin()` to change affinity and trigger migration
- Monitor via `/proc/schedstat` reads or `on_sched_softirq_end()` callback during load balancing
- Verify bug: `lb_hot_gained` increments without corresponding task migrations
- Detection: Compare `lb_hot_gained` vs actual hot task movement between CPUs
- Look for `nr_failed_migrations_hot` not being updated despite hot tasks remaining on source CPU
