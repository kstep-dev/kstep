# RT: Push Migration Race With migrate_disable()

**Commit:** `feffe5bb274dd3442080ef0e4053746091878799`
**Affected files:** `kernel/sched/rt.c`, `kernel/sched/deadline.c`
**Fixed in:** v6.4-rc1
**Buggy since:** v5.11-rc1 (commit `95158a89dd50` — "sched,rt: Use the full cpumask for balancing")

## Bug Description

The RT (and SCHED_DEADLINE) task push mechanism contains a time-of-check-time-of-use (TOCTOU) race on the `is_migration_disabled()` property of a task being pushed to another CPU. When an RT runqueue is overloaded (more than one RT task queued), `push_rt_task()` selects the highest-priority pushable task and attempts to migrate it to a lower-priority CPU. Before initiating the migration, it checks whether the task has migration disabled via `is_migration_disabled(next_task)`. However, this check occurs before the function `find_lock_lowest_rq()` is called, and `find_lock_lowest_rq()` may temporarily release and reacquire the source runqueue lock via `double_lock_balance()`. During this lock-release window, the task's migration-disabled state can change.

The root cause commit `95158a89dd50` ("sched,rt: Use the full cpumask for balancing") was introduced as part of the `migrate_disable()` support for RT tasks. That commit changed `find_lowest_rq()` to use the full `cpus_mask` instead of `cpus_ptr` when selecting a target CPU for push migration, allowing migrate-disabled tasks to be considered for pushing. The intent was to enable the push mechanism to push the *current* running task away from a CPU that has a migrate-disabled lower-priority task. However, the associated function `find_lock_lowest_rq()`, which performs the actual double-lock acquisition, did not add a re-validation check for `is_migration_disabled()` after reacquiring the locks.

The prior related fix `a7c81556ec4d` ("sched: Fix migrate_disable() vs rt/dl balancing") added the initial `is_migration_disabled()` check at the `retry:` label in `push_rt_task()`, which handles the case where the task is already migration-disabled when push_rt_task first examines it. But it missed the case where the task transitions to migration-disabled *during* the double lock acquisition.

An identical race exists in the SCHED_DEADLINE push path, where `push_dl_task()` calls `find_lock_later_rq()`, which has the same pattern of temporarily releasing the source runqueue lock without re-validating the migration-disabled state after reacquisition.

## Root Cause

The race manifests in the following code path in `push_rt_task()` (kernel/sched/rt.c, lines ~2053–2170 before the fix):

1. `push_rt_task()` calls `pick_next_pushable_task(rq)` to select the highest-priority pushable RT task (`next_task`) from the current runqueue. At this point, the source rq lock is held.

2. At the `retry:` label, `push_rt_task()` checks `is_migration_disabled(next_task)`. If the task has migration disabled, it takes the alternative path (pushing the current task instead via `stop_one_cpu_nowait`). If migration is NOT disabled, it proceeds to step 3.

3. `push_rt_task()` calls `find_lock_lowest_rq(next_task, rq)` to find and lock the target runqueue. Inside this function, `double_lock_balance(rq, lowest_rq)` is called. When `double_lock_balance` returns 1 (indicating the source lock was released and reacquired), the function validates that the task hasn't changed state during the window. The validation checks are:
   ```c
   if (unlikely(task_rq(task) != rq ||
                !cpumask_test_cpu(lowest_rq->cpu, &task->cpus_mask) ||
                task_on_cpu(rq, task) ||
                !rt_task(task) ||
                !task_on_rq_queued(task)))
   ```
   Critically, `is_migration_disabled(task)` is **absent** from this list.

4. During the window between `raw_spin_rq_unlock(this_rq)` at line 2620 of sched.h and the completion of `double_rq_lock(this_rq, busiest)`, the task's state can change. Specifically, another CPU could wake the task, the task could be scheduled on the source CPU (if preemption is enabled after the lock release), it could call `migrate_disable()`, and then get preempted back to a non-running state.

5. After `find_lock_lowest_rq()` returns a non-NULL `lowest_rq`, `push_rt_task()` proceeds unconditionally:
   ```c
   deactivate_task(rq, next_task, 0);
   set_task_cpu(next_task, lowest_rq->cpu);
   ```
   Inside `set_task_cpu()` (kernel/sched/core.c line 3205), the assertion `WARN_ON_ONCE(is_migration_disabled(p))` fires because the task now has migration disabled.

The same pattern exists in `find_lock_later_rq()` in kernel/sched/deadline.c (line ~2244), where the re-validation after `double_lock_balance` also omits the `is_migration_disabled()` check. The `push_dl_task()` function has an identical pre-check for migration disabled but the same TOCTOU gap.

The `double_lock_balance()` function (sched.h lines 2615–2654) has two implementations depending on CONFIG_PREEMPTION. In the preemptible version, it always releases and reacquires the source lock (returning 1). In the non-preemptible "unfair" version, it first attempts a trylock on the target and only releases the source lock if the trylock fails and lock ordering requires it. The race is more likely to occur with CONFIG_PREEMPTION enabled, since the lock release always happens.

## Consequence

The most immediate observable consequence is a `WARN_ON_ONCE` triggered in `set_task_cpu()` at kernel/sched/core.c line 3205:
```
WARNING: ... at kernel/sched/core.c:3205 set_task_cpu+0x.../0x...
```
This warning fires because a task with `migration_disabled > 0` is being forcibly migrated to another CPU.

Beyond the warning, migrating a migration-disabled task can lead to serious correctness violations. The `migrate_disable()` API is a promise to the task that it will remain on its current CPU for the duration of the critical section. Code running under `migrate_disable()` may rely on per-CPU data, per-CPU variables, or CPU-local resources that become invalid on a different CPU. Forcibly migrating such a task can cause:
- **Data corruption** of per-CPU data structures (e.g., per-CPU counters, per-CPU caches, or per-CPU allocator state) that the task was accessing.
- **Use-after-free or stale pointer dereferences** if the task holds references to CPU-local resources.
- On PREEMPT_RT kernels, where `migrate_disable()` replaces `preempt_disable()` for many critical sections, this can break fundamental kernel invariants and lead to **kernel panics, deadlocks, or silent memory corruption**.

The bug affects both SCHED_FIFO/SCHED_RR (via `push_rt_task` / `find_lock_lowest_rq`) and SCHED_DEADLINE (via `push_dl_task` / `find_lock_later_rq`) task classes. Any system running RT or deadline tasks that use `migrate_disable()` is potentially affected. The race window is narrow (the duration of the lock release in `double_lock_balance`), so the bug may be rarely triggered under light loads but becomes more probable under heavy RT workloads with frequent push migrations.

## Fix Summary

The fix adds `is_migration_disabled(task)` to the re-validation check inside both `find_lock_lowest_rq()` (rt.c) and `find_lock_later_rq()` (deadline.c). Specifically, after `double_lock_balance()` returns 1 (indicating the source lock was released and reacquired), the function now checks:

```c
if (unlikely(task_rq(task) != rq ||
             !cpumask_test_cpu(lowest_rq->cpu, &task->cpus_mask) ||
             task_on_cpu(rq, task) ||
             !rt_task(task) ||
             is_migration_disabled(task) ||   /* NEW CHECK */
             !task_on_rq_queued(task)))
```

If `is_migration_disabled(task)` is true after reacquiring the lock, the function releases both locks and returns NULL to `push_rt_task()`. This causes `push_rt_task()` to either retry with a different task or give up, preventing the invalid migration.

The fix is correct because it closes the TOCTOU gap: the migration-disabled state is now validated at the exact point where both runqueue locks are held simultaneously, ensuring no further state changes can occur between the check and the actual migration. The existing checks for `task_rq(task) != rq` (task migrated away), `!cpumask_test_cpu(...)` (affinity changed), `task_on_cpu(rq, task)` (task started running), `!rt_task(task)` (scheduling class changed), and `!task_on_rq_queued(task)` (task dequeued) already cover other state transitions that can occur during the lock release window — the migration-disabled check was simply missing from this comprehensive list.

The identical fix is applied to `find_lock_later_rq()` in deadline.c for the parallel SCHED_DEADLINE push path, which had the same omission. The comment in `find_lock_lowest_rq()` is also updated to mention the migration-disabled race explicitly.

## Triggering Conditions

The following conditions must all be met to trigger the bug:

- **Multiple CPUs (≥2):** The push mechanism requires at least two CPUs — one that is "overloaded" with RT tasks (the source) and one with lower-priority tasks (the target). The `find_lock_lowest_rq` function acquires a double lock between the source and target runqueues.

- **RT overload on source CPU:** The source CPU must have `rq->rt.overloaded` set to true, meaning it has more than one runnable RT task. This triggers `push_rt_task()` to attempt migrating the lower-priority RT task.

- **Target CPU with lower priority:** There must be a CPU running a task with strictly lower priority than the task being pushed (i.e., `lowest_rq->rt.highest_prio.curr > task->prio`).

- **double_lock_balance releases the source lock:** This happens either (a) always, when CONFIG_PREEMPTION is enabled, or (b) when the trylock on the target rq fails and lock ordering requires release-and-reacquire. Condition (a) is the common case on preemptible kernels.

- **Task transitions to migration-disabled during lock release:** The task selected for pushing must call `migrate_disable()` during the brief window when the source runqueue lock is released inside `double_lock_balance()`. This requires the task to be scheduled on the source CPU (or have its `migration_disabled` counter incremented by some mechanism) during this window. Concretely:
  1. The task is queued but not running on the source CPU (it's on the pushable list).
  2. During the lock release window, the higher-priority task on the source CPU yields or blocks.
  3. The pushable task gets scheduled, runs, and calls `migrate_disable()`.
  4. A higher-priority task preempts it back before the lock is reacquired.
  5. Now the task is queued, not running, but has `migration_disabled > 0`.

- **Kernel configuration:** CONFIG_SMP must be enabled (push/pull logic is SMP-only). CONFIG_PREEMPTION (PREEMPT or PREEMPT_RT) makes the race much more likely since `double_lock_balance` always releases the source lock. On non-preemptible kernels, the lock may not be released at all (if the trylock succeeds), making the race much harder to trigger.

- **The race is probabilistic:** The lock-release window in `double_lock_balance` is very brief (the time between `raw_spin_rq_unlock` and the completion of `double_rq_lock`). The task must transition to migration-disabled during this exact window. Under heavy RT load with frequent push attempts, the probability increases but remains low per individual attempt.

## Reproduce Strategy (kSTEP)

The reproduction strategy aims to create conditions where the TOCTOU race in `push_rt_task` / `find_lock_lowest_rq` can be triggered. Since this is an inherently probabilistic race, the strategy focuses on maximizing the number of push attempts while having the target task rapidly toggle `migrate_disable()`.

### CPU Topology

Configure QEMU with at least 3 CPUs (CPU0 for the kSTEP driver, CPU1 and CPU2 for the workload). Set `num_cpus = 3` in the driver configuration.

### Task Setup

1. **Create a custom kthread `T_target`** (directly via `kthread_create` in the driver, not through `kstep_kthread_create`) that runs the following loop:
   ```c
   while (!kthread_should_stop()) {
       migrate_disable();
       /* Brief spin to hold migration disabled */
       for (int i = 0; i < 100; i++)
           cpu_relax();
       migrate_enable();
       /* Brief spin with migration enabled */
       for (int i = 0; i < 10; i++)
           cpu_relax();
   }
   ```
   Set `T_target` to SCHED_FIFO with a moderate priority (e.g., priority 50, which means `MAX_RT_PRIO - 1 - 50 = 48` in kernel terms). Pin it to CPU1 using `set_cpus_allowed_ptr()` with a mask covering CPU1 and CPU2 (the task must have `nr_cpus_allowed > 1` to be pushable).

2. **Create an RT kthread `T_high`** using `kstep_kthread_create()`, set to SCHED_FIFO with a higher priority than `T_target` (e.g., priority 10, kernel prio 38). Pin `T_high` to CPU1 using `kstep_kthread_bind()`. This task spins on CPU1, making `T_target` the pushable task. Start it with `kstep_kthread_start()`.

3. **CPU2 should have only CFS tasks or no RT tasks**, ensuring it's the lowest-priority CPU that `find_lowest_rq` will select as the push target. The default idle task suffices.

4. Optionally create a CFS task on CPU2 to ensure the cpupri data is populated.

### Triggering the Push Mechanism

The RT overload mechanism is triggered automatically when both `T_high` and `T_target` are runnable on CPU1. The kernel's `push_rt_tasks()` function (called via `balance_rt` or `rto_push_irq_work_func`) will attempt to push `T_target` to CPU2.

To increase the frequency of push attempts:
- Use `kstep_tick_repeat(N)` with a large N (e.g., 10000) to generate many scheduler ticks, each of which may trigger the push mechanism.
- Alternatively, have `T_high` intermittently block and wake using `kstep_kthread_block()` and a timer-based wakeup, which creates repeated enqueue/dequeue events that trigger push_rt_tasks.

### The Race Window

During each push attempt in `find_lock_lowest_rq()`:
1. The source rq lock (CPU1) is released in `double_lock_balance`.
2. If `T_high` happens to block at this instant, `T_target` gets scheduled.
3. `T_target` calls `migrate_disable()` (it's in a tight toggle loop).
4. `T_high` wakes back up and preempts `T_target`.
5. `find_lock_lowest_rq` reacquires the lock, sees the task is still queued, still RT, still on the right rq — but doesn't check `is_migration_disabled`.
6. `push_rt_task` proceeds to call `set_task_cpu`, hitting `WARN_ON_ONCE`.

To maximize the probability of hitting the race:
- Make `T_target`'s migrate_disable/enable cycle as tight as possible (maximize the fraction of time it has migration disabled).
- Make push attempts as frequent as possible.
- Have `T_high` block/unblock rapidly to create scheduling churn on CPU1.

### Detection

Check for the bug by monitoring kernel log output (via `dmesg` or `printk` output in kSTEP logs) for the warning:
```
WARNING: ... set_task_cpu ... is_migration_disabled
```

Alternatively, use the `on_sched_softirq_end` callback to read `T_target`'s `migration_disabled` counter and current CPU. If `task_cpu(T_target)` changes while `T_target->migration_disabled > 0`, the bug has been triggered. Report this with `kstep_fail("task migrated while migration disabled: cpu=%d, migration_disabled=%d")`.

On the fixed kernel, `find_lock_lowest_rq()` will detect the migration-disabled state after reacquiring the lock and abort the push, so `T_target` should never be migrated while it has migration disabled.

### kSTEP Framework Considerations

No modifications to the core kSTEP framework are strictly necessary. The driver can create its own kthread directly using `kthread_create()` from `<linux/kthread.h>`, set its scheduling policy with `sched_setscheduler_nocheck()`, and pin it with `set_cpus_allowed_ptr()`. These are all standard kernel APIs available in a kernel module.

However, if a reusable pattern is desired, a minor kSTEP extension could add a `kstep_kthread_migrate_toggle()` action that repeatedly calls `migrate_disable()` / `migrate_enable()` in a tight loop. This would be a simple addition to `kmod/kthread.c` following the existing action pattern (`do_spin`, `do_yield`, etc.).

### Expected Behavior

- **Buggy kernel (pre-fix):** After sufficiently many ticks (potentially thousands), the WARN_ON_ONCE in `set_task_cpu()` should fire, and/or `T_target` should be observed on a different CPU while its `migration_disabled` counter is nonzero. The test reports `kstep_fail`.
- **Fixed kernel (post-fix):** The additional check in `find_lock_lowest_rq()` prevents the migration. `T_target` is never migrated while migration-disabled. The WARN_ON_ONCE never fires. After all ticks complete without observing the bug, the test reports `kstep_pass`.

### Reliability Notes

This is a probabilistic race reproduction. The race window is very narrow (the lock-release duration in `double_lock_balance`), so reproduction may require many iterations. If the race cannot be reliably triggered in a reasonable number of ticks, consider:
1. Adding a small `udelay()` or `cpu_relax()` loop inside `find_lock_lowest_rq` (only in the buggy kernel, as a test aid) to widen the race window.
2. Using `KSYM_IMPORT` to access `push_rt_task` internals and add instrumentation.
3. Increasing the test duration significantly (100,000+ ticks).

The race is more likely on PREEMPT kernels (which kSTEP typically uses) since `double_lock_balance` always releases the source lock in that configuration.
