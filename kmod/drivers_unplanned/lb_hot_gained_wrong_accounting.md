# LB: Incorrect schedstat accounting for hot task migrations

**Commit:** `a430d99e349026d53e2557b7b22bd2ebd61fe12a`
**Affected files:** `kernel/sched/fair.c`, `include/linux/sched.h`
**Fixed in:** v6.14-rc1
**Buggy since:** v3.10-rc1 (introduced by commit `d31980846f96` "sched: Move up affinity check to mitigate useless redoing overhead")

## Bug Description

The `/proc/schedstat` interface exposes per-sched-domain statistics about load balancing activity. Among these, `lb_hot_gained[idle_type]` tracks the number of cache-hot tasks that were successfully pulled (migrated) during load balancing, and per-task `nr_forced_migrations` tracks how many times a task was force-migrated despite being cache-hot. These counters are consumed by userspace performance monitoring tools such as `perf sched stats`.

The bug is that these two counters (`lb_hot_gained` and `nr_forced_migrations`) were incremented prematurely in `can_migrate_task()` — the function that determines whether a task *may* be migrated — rather than at the point where the task is actually detached from the source runqueue. After `can_migrate_task()` returns 1 (indicating the task is eligible for migration), the caller `detach_tasks()` performs additional checks based on the migration type (load, utilization, task count, or misfit) and may decide NOT to actually migrate the task (jumping to the `next:` label). When this happens, the hot-task counters are inflated because they were incremented for tasks that were never actually migrated.

Additionally, when a hot task passes `can_migrate_task()` but is subsequently skipped in `detach_tasks()`, the per-task `nr_failed_migrations_hot` counter is NOT incremented, even though the migration effectively failed. This means this counter is under-counted.

The net effect is that `/proc/schedstat` reports inaccurate data: `lb_hot_gained` is over-counted, `nr_forced_migrations` is over-counted, and `nr_failed_migrations_hot` is under-counted. This misleads performance analysis tools and developers attempting to diagnose load balancing behavior.

## Root Cause

The root cause lies in the placement of `schedstat_inc()` calls within `can_migrate_task()` (in `kernel/sched/fair.c`). In the buggy code, when `can_migrate_task()` determines a task is cache-hot but still eligible for migration (either because `tsk_cache_hot <= 0` or because `nr_balance_failed > cache_nice_tries`), it immediately increments the statistics:

```c
if (tsk_cache_hot <= 0 ||
    env->sd->nr_balance_failed > env->sd->cache_nice_tries) {
    if (tsk_cache_hot == 1) {
        schedstat_inc(env->sd->lb_hot_gained[env->idle]);  /* PREMATURE */
        schedstat_inc(p->stats.nr_forced_migrations);       /* PREMATURE */
    }
    return 1;
}
```

However, `can_migrate_task()` is called from two locations: `detach_one_task()` and `detach_tasks()`. In `detach_one_task()`, the task is unconditionally migrated after `can_migrate_task()` returns 1, so the early increment is harmless. But in `detach_tasks()`, after `can_migrate_task()` returns 1, the task must pass a second round of checks in a `switch (env->migration_type)` block:

- **`migrate_load`**: The task's `task_h_load()` (hierarchical load) is compared against `env->imbalance`. If `shr_bound(load, env->sd->nr_balance_failed) > env->imbalance`, the task is skipped (`goto next`). This happens when a task's load exceeds the remaining imbalance to resolve.
- **`migrate_util`**: Similarly, `task_util_est(p)` is compared against `env->imbalance`. Heavy-utilization tasks may be skipped.
- **`migrate_load` with `LB_MIN`**: If `load < 16` and the balancer hasn't failed yet, the task is skipped.
- **`migrate_misfit`**: If the task fits on the source CPU (`task_fits_cpu(p, env->src_cpu)`), it is skipped.

In all these `goto next` cases, the task is NOT detached, but its stats were already incremented in `can_migrate_task()`. Furthermore, when these hot tasks are skipped at the `next:` label, the `nr_failed_migrations_hot` counter is not incremented — a second bookkeeping error, since the migration of a hot task effectively failed.

The original commit `d31980846f96` (from 2013) restructured `can_migrate_task()` to move the affinity check up and the cache-hot evaluation further down, and placed the stats increment in `can_migrate_task()`. At that time, the code path after `can_migrate_task()` returned 1 was simpler and the task was more likely to be actually migrated. As the load balancer grew more sophisticated with additional `goto next` checks in `detach_tasks()`, this premature increment became increasingly problematic.

## Consequence

The observable impact is **incorrect statistics** in `/proc/schedstat` and per-task scheduling statistics. Specifically:

1. **`lb_hot_gained[idle_type]`** (per sched-domain): Over-reported. Tasks counted as "hot gained" may never have been actually migrated. Tools like `perf sched stats` that report "hot tasks pulled" during load balancing show inflated numbers, potentially leading developers to believe there is more aggressive hot-task migration than actually occurs.

2. **`nr_forced_migrations`** (per task): Over-reported. A task's forced migration count includes migrations that were decided against in `detach_tasks()`. This can mislead cache-aware performance tuning.

3. **`nr_failed_migrations_hot`** (per task): Under-reported. When a hot task passes `can_migrate_task()` but is then skipped in `detach_tasks()`, this counter is not incremented even though the migration failed. This hides the true frequency of hot-task migration failures.

While this bug does not cause crashes, hangs, or incorrect scheduling decisions (the actual task migration behavior is correct), it corrupts the observability data that administrators and developers rely on to understand and tune scheduler behavior. In production environments, incorrect schedstat data can lead to wrong conclusions about load balancing effectiveness, cache behavior, and NUMA locality, potentially resulting in misguided performance tuning efforts.

## Fix Summary

The fix introduces a new single-bit flag `sched_task_hot` in `struct task_struct` (in `include/linux/sched.h`) to carry the "this task is cache-hot" information from `can_migrate_task()` to the actual migration point. The approach works in three parts:

**In `can_migrate_task()`**: Instead of immediately incrementing `lb_hot_gained` and `nr_forced_migrations` when a hot task is deemed migratable, the function now sets `p->sched_task_hot = 1`. At the top of the function, any stale `sched_task_hot` flag from a previous iteration is cleared (`p->sched_task_hot = 0`), ensuring the flag is only set for the current load balancing pass.

**In `detach_task()`**: This function is called only when a task is actually being detached (migrated). It now checks `p->sched_task_hot`, and only if set, increments `env->sd->lb_hot_gained[env->idle]` and `p->stats.nr_forced_migrations`, then clears the flag. This ensures the counters are only incremented for tasks that are truly migrated.

**In `detach_tasks()` at the `next:` label**: When a task that passed `can_migrate_task()` is skipped by the migration-type checks, the fix checks `p->sched_task_hot` and increments `p->stats.nr_failed_migrations_hot` if set. This correctly accounts for hot tasks whose migration was attempted but ultimately abandoned.

This fix is correct and complete because it precisely decouples the decision "this task is hot and could be migrated" (which happens in `can_migrate_task()`) from the action "this task was actually migrated" (which happens in `detach_task()`). The `sched_task_hot` flag acts as a deferred notification mechanism. The `detach_one_task()` path also works correctly because it calls `detach_task()` (which checks and clears the flag) immediately after `can_migrate_task()` returns 1.

## Triggering Conditions

The bug triggers whenever the following conditions align during a CFS load balancing pass:

1. **Load imbalance exists**: There must be a load (or utilization or task count) imbalance between two CPUs or scheduling groups that triggers load balancing via `sched_balance_rq()`.

2. **Cache-hot task passes `can_migrate_task()`**: A task on the source runqueue must be cache-hot (`task_hot()` returns 1, meaning `rq_clock_task(src_rq) - p->se.exec_start < sysctl_sched_migration_cost`) but still eligible for migration — either because the task is cold according to `migrate_degrades_locality()`, or because `env->sd->nr_balance_failed > env->sd->cache_nice_tries` (forced migration after many failed balance attempts).

3. **Task is skipped in `detach_tasks()`**: After `can_migrate_task()` returns 1, the task must hit `goto next` in the `switch (env->migration_type)` block of `detach_tasks()`. The most common scenario for this is:
   - **`migrate_load`**: The task's `task_h_load()` (potentially shifted right by `nr_balance_failed`) exceeds the remaining `env->imbalance`. This happens when the remaining imbalance is small and the task is relatively heavy.
   - **`migrate_util`**: The task's estimated utilization exceeds the remaining imbalance.
   - **`migrate_misfit`**: The task fits on the source CPU.

4. **`CONFIG_SCHEDSTATS` must be enabled**: The schedstat counters are only active when this config option is enabled (which it typically is on development/testing kernels and many distribution kernels).

5. **NUMA balancing not actively degrading locality**: If `migrate_degrades_locality()` returns a value other than -1, it participates in the hot/cold decision. On non-NUMA systems, it always returns -1, and `task_hot()` alone determines cache-hotness.

The simplest concrete scenario: CPU 1 has 4 CFS tasks, CPU 2 has 0. Three tasks are cold (large `exec_start` delta), one is hot (recently ran). The hot task has a relatively large load. Load balancing triggers with `migrate_load` type. The cold tasks are migrated first, reducing `env->imbalance`. When the hot task is processed, its load exceeds the remaining imbalance, so it hits `goto next` — but its stats were already incorrectly incremented. This scenario can occur frequently on multi-CPU systems under moderate load.

## Reproduce Strategy (kSTEP)

This bug is a **statistics-only** bug affecting `/proc/schedstat` counters. While the incorrect counters are observable programmatically via `struct sched_domain` and `struct task_struct` scheduler statistics fields, reproducing and verifying this bug in kSTEP requires reading internal scheduler statistics that kSTEP can access, but the core challenge is that **this is purely an observability/accounting bug with no effect on actual scheduling behavior**.

kSTEP's strengths are in triggering and observing scheduling behavior (task placement, migration, preemption). To reproduce this bug, we would need to:

1. **Trigger load balancing with hot tasks**: Create multiple CFS tasks on one CPU to cause load imbalance. Make some tasks cache-hot by having them run recently (small delta between `rq_clock_task` and `exec_start`). This is partially controllable in kSTEP by waking tasks and ticking, but precise control of `exec_start` relative to the rq clock at the moment load balancing runs is difficult because kSTEP's tick mechanism advances the clock in ways that may or may not align with exec_start timestamps.

2. **Force a hot task to pass `can_migrate_task()` but fail `detach_tasks()` checks**: This requires the hot task to have `nr_balance_failed > cache_nice_tries` (forced migration path) AND the task's load to exceed the remaining imbalance in `detach_tasks()`. Controlling `nr_balance_failed` requires triggering multiple failed balance attempts, which is hard to orchestrate deterministically. The `cache_nice_tries` value depends on the sched_domain topology level.

3. **Read and compare schedstat counters**: kSTEP can access `struct sched_domain` via `cpu_rq(cpu)->sd` and read `sd->lb_hot_gained[idle]`. It can also access `p->stats.nr_forced_migrations` and `p->stats.nr_failed_migrations_hot`. However, interpreting these values correctly requires correlating them with actual migration events, which would require either hooking into `detach_task()` or counting migrations independently.

**Why this bug CANNOT be reliably reproduced with kSTEP:**

The fundamental issue is that this bug manifests as **incorrect counter values** rather than incorrect scheduling behavior. To detect the bug, we would need to:
- Count the exact number of hot tasks that `can_migrate_task()` returned 1 for (this is where stats are incremented on buggy kernels)
- Count the exact number of hot tasks that were actually detached in `detach_task()`
- Compare these two counts with the schedstat values

This comparison requires instrumenting internal load balancing functions (`can_migrate_task`, `detach_tasks`, `detach_task`) to count events as they happen, then comparing against the schedstat counters. kSTEP does not currently have hooks into these specific functions. While kSTEP has `on_sched_balance_begin` and `on_sched_balance_selected` callbacks, these fire at a higher level (before the per-task iteration in `detach_tasks()`) and cannot observe individual task migration decisions.

Furthermore, controlling whether a task is "cache-hot" at the precise moment of load balancing requires fine-grained control over when tasks last executed relative to the rq clock at balance time. kSTEP's `kstep_tick()` advances the clock and may trigger load balancing, but the task's `exec_start` depends on when the task was last picked by the scheduler, which is not directly controllable. The `sysctl_sched_migration_cost` threshold (default 500µs) is small relative to kSTEP's tick granularity, making it unreliable to guarantee a task is hot vs cold at the exact moment the load balancer evaluates it.

Even if we set `sysctl_sched_migration_cost` to a very large value (making all tasks hot), we still need to trigger the specific code path where a hot task passes `can_migrate_task()` but fails the load/util check in `detach_tasks()`. This requires precise control over `env->imbalance` values and task loads, which depend on the complex `find_busiest_group()`/`find_busiest_queue()` calculations that kSTEP cannot directly manipulate.

**What would need to be added to kSTEP:**

To support this class of schedstat-accounting bugs, kSTEP would need:
- A callback hook inside `detach_tasks()` or `can_migrate_task()` to observe per-task migration decisions (e.g., `on_task_migrate_decision(task, accepted)`)
- A callback inside `detach_task()` to observe actual detachments (e.g., `on_task_detached(task)`)
- A way to read and compare schedstat counter values before and after load balancing
- Better control over task `exec_start` to reliably make tasks cache-hot or cache-cold

These are not minor extensions — they require adding new callback points deep inside the load balancing hot path, which could affect the very behavior being tested.

**Alternative reproduction methods:**

The simplest way to reproduce this outside kSTEP is on a real multi-CPU system:
1. Enable `CONFIG_SCHEDSTATS` 
2. Run a workload that creates load imbalance with some hot tasks (e.g., a mix of CPU-bound and intermittently-active tasks)
3. Monitor `/proc/schedstat` `lb_hot_gained` values and compare against actual observed migrations via `ftrace` (`sched:sched_migrate_task` tracepoint)
4. On buggy kernels, `lb_hot_gained` will be strictly greater than the actual count of hot tasks observed in migration tracepoints
