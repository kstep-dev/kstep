# sched: Fix do_set_cpus_allowed() locking

- **Commit:** abfc01077df66593f128d966fdad1d042facc9ac
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

The function `__do_set_cpus_allowed()` requires both `p->pi_lock` and `rq->lock` to be held for safe operation, but callers of `do_set_cpus_allowed()` were only holding `p->pi_lock`. This locking violation creates a race condition where concurrent operations could modify the task's cpumask and scheduling state without proper synchronization, potentially leading to task scheduling state corruption.

## Root Cause

The function `__do_set_cpus_allowed()` performs critical operations on the task runqueue (dequeue/enqueue, cpumask updates) that require both the task's `pi_lock` and the runqueue's `rq->lock` to prevent concurrent modifications. However, `do_set_cpus_allowed()` was calling this function with only `p->pi_lock` held, violating the locking contract and creating a window for race conditions.

## Fix Summary

The fix wraps `do_set_cpus_allowed()` with a `scoped_guard(__task_rq_lock, p)` to ensure both `rq->lock` and `p->pi_lock` are held before calling `__do_set_cpus_allowed()`. Additionally, the lockdep assertion in `__do_set_cpus_allowed()` is moved to the function entry point to unconditionally require both locks, eliminating the locking inconsistency.

## Triggering Conditions

- **Scheduler subsystem**: Core task affinity management in `kernel/sched/core.c`
- **Code path**: `do_set_cpus_allowed()` → `__do_set_cpus_allowed()` with insufficient locking
- **Required state**: Active task with queued/running state during cpumask modification
- **Race window**: Concurrent operations modifying task runqueue state while only `p->pi_lock` is held
- **Timing condition**: Task dequeue/enqueue operations during cpumask updates without `rq->lock`
- **Key trigger**: Calls to `do_set_cpus_allowed()` from contexts like `select_fallback_rq()` or kthread binding
- **Observable effects**: Task scheduling state corruption, potential runqueue inconsistencies

## Reproduce Strategy (kSTEP)

- **CPUs needed**: Minimum 3 (driver on CPU 0, tasks on CPUs 1-2)
- **Setup**: Create multiple tasks with different CPU affinities using `kstep_task_create()` and `kstep_task_pin()`
- **Topology**: Use `kstep_topo_init()` and `kstep_topo_set_cls()` to create CPU domains for migration triggers
- **Trigger sequence**: 
  1. Pin tasks to specific CPUs with `kstep_task_pin()` 
  2. Use `kstep_tick_repeat()` to build runqueue state
  3. Force affinity changes via CPU hotplug simulation or fallback selection
  4. Monitor with `on_tick_begin()` callback using `kstep_output_nr_running()`
- **Detection**: Check for runqueue inconsistencies, task state corruption, or lockdep warnings
- **Validation**: Compare behavior before/after the locking fix with identical test sequences
