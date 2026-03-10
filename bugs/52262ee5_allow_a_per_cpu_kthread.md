# sched/fair: Allow a per-CPU kthread waking a task to stack on the same CPU, to fix XFS performance regression

- **Commit:** 52262ee567ad14c9606be25f3caddcefa3c514e4
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS

## Bug Description

When XFS transitioned from bound workqueues to unbound workqueues, it triggered a significant performance regression in dbench (up to 48.53% regression in some configurations). The issue manifested as excessive CPU migrations during IO completion, where tasks would ping-pong between CPUs instead of staying on their current CPU, leading to poor cache locality and increased communication costs.

## Root Cause

The scheduler's `select_idle_sibling()` function did not recognize the strong relationship between a bound kworker (IO completion handler) and the task it was waking. When the kworker woke a task that had queued work for it, the scheduler treated this as a regular wakeup and migrated the task away from the kworker's CPU to find an "idle" CPU. This caused tasks to migrate across CPUs within the same LLC on nearly every IO completion, creating a round-robin pattern with poor performance implications.

## Fix Summary

The fix adds special-case logic in `select_idle_sibling()` to detect when a per-CPU kthread (the current task) is waking another task on the same CPU where the task previously ran. If the kworker's runqueue has only itself running (`nr_running <= 1`), the task is allowed to stack on the same CPU rather than migrate. This recognizes the transactional relationship between the kworker and task in IO completion scenarios, expecting the kworker to sleep shortly after.

## Triggering Conditions

The bug occurs in the `select_idle_sibling()` path within CFS when a per-CPU kthread wakes a task. Specifically:
- Current task must be a per-CPU kthread (`PF_KTHREAD` flag set, `nr_cpus_allowed == 1`)
- The kthread must be waking another task that previously ran on the same CPU (`prev == smp_processor_id()`)
- The current CPU's runqueue should have low contention (`nr_running <= 1`, typically just the kworker itself)
- The wakee task must have CPU affinity allowing it to run on the current CPU
- Without the fix, `select_idle_sibling()` migrates the task away from the kworker's CPU to find an "idle" CPU
- This creates ping-pong migration patterns on every IO completion, especially in workloads with bound kworkers handling IO completion

## Reproduce Strategy (kSTEP)

Setup requires at least 2 CPUs (CPU 1 for kworker, others for migration targets). In `setup()`: create a bound kthread on CPU 1 using `kstep_kthread_create()` and `kstep_kthread_bind()`, and a regular task with `kstep_task_create()`. In `run()`: start the kthread with `kstep_kthread_start()`, wake the task on CPU 1 with `kstep_task_wakeup()`, then let it run with `kstep_tick_repeat(10)`. Next, pause the task with `kstep_task_pause()` and tick until it blocks. Finally, have the kthread wake the task using `kstep_kthread_syncwake()`. Without the fix, `select_idle_sibling()` should migrate the task away from CPU 1 despite the kworker relationship. Use `on_tick_end` callback with `kstep_output_curr_task()` to track task CPU placement and detect the migration pattern. Check `cpu_rq(1)->nr_running` and current task locations to verify the bug reproduction.
