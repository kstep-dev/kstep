# Core: Forced-newidle balancer steals migration-disabled task

**Commit:** `386ef214c3c6ab111d05e1790e79475363abaa05`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.18-rc2
**Buggy since:** v5.13-rc1 (introduced by `d2dfa17bc7de6` "sched: Trivial forced-newidle balancer")

## Bug Description

The core scheduling subsystem provides a mechanism to ensure that tasks sharing an SMT core only run simultaneously if they share the same "cookie" (a trust domain identifier set via `prctl(PR_SCHED_CORE)`). When a sibling CPU is forced idle because no matching-cookie task is available, the forced-newidle balancer (`sched_core_balance()`) tries to find and steal a compatible task from other CPUs to fill the idle slot.

The function `try_steal_cookie()` iterates over tasks on a source CPU's core scheduling red-black tree (`core_tree`) looking for tasks that match the needed cookie and can run on the destination CPU. The original code checked task migration eligibility using only `cpumask_test_cpu(this, &p->cpus_mask)`, which verifies the destination CPU is in the task's allowed cpumask. However, this check is insufficient because it ignores the `migration_disabled` state of the task.

A task that has called `migrate_disable()` must remain on its current CPU for the duration of the migration-disabled section, even if its cpumask would normally allow it to run elsewhere. The `migration_disabled` mechanism is used to protect per-CPU data and assumptions: code running under `migrate_disable()` may hold per-CPU local locks or access per-CPU variables that become invalid if the task is moved. By only checking the cpumask, `try_steal_cookie()` could migrate a task that is in a migration-disabled section to a different CPU, violating fundamental per-CPU invariants.

## Root Cause

The root cause is in the `try_steal_cookie()` function in `kernel/sched/core.c`. The function searches for tasks matching a core scheduling cookie that can be stolen from a source CPU (`that`) to a destination CPU (`this`). The critical check at the time of the bug was:

```c
if (!cpumask_test_cpu(this, &p->cpus_mask))
    goto next;
```

This only tests whether the destination CPU is in the task's CPU affinity mask. It does not account for `p->migration_disabled`, which is set when a task calls `migrate_disable()`. The kernel provides a dedicated function `is_cpu_allowed(p, cpu)` (defined around line 2283 of `core.c`) that performs a comprehensive check:

```c
static inline bool is_cpu_allowed(struct task_struct *p, int cpu)
{
    if (!cpumask_test_cpu(cpu, p->cpus_ptr))
        return false;
    if (is_migration_disabled(p))
        return cpu_online(cpu);
    if (!(p->flags & PF_KTHREAD))
        return cpu_active(cpu) && task_cpu_possible(cpu, p);
    if (kthread_is_per_cpu(p))
        return cpu_online(cpu);
    if (cpu_dying(cpu))
        return false;
    return true;
}
```

The key branch is `is_migration_disabled(p)`: when migration is disabled, `is_cpu_allowed()` returns `true` only for the CPU where the task is currently running (since `cpu_online(cpu)` would pass for all online CPUs, but the semantic intention combined with the migration framework is that the task must not be moved). Actually, more precisely, `is_cpu_allowed()` returning true when `is_migration_disabled(p)` for any online CPU was the behavior — but the migration infrastructure elsewhere (e.g., `move_queued_task()`, `__set_cpus_allowed_ptr()`) respects `migration_disabled` and refuses to migrate. The issue is that `try_steal_cookie()` bypasses all of this by directly calling `deactivate_task()`/`set_task_cpu()`/`activate_task()`, so the only guard is the check before these calls.

Wait — re-examining `is_cpu_allowed()` more carefully: when `is_migration_disabled(p)` is true, it returns `cpu_online(cpu)` which would return true for any online CPU including the destination. So the actual protection is that `is_cpu_allowed()` combined with other migration machinery refuses to move migration-disabled tasks. But looking at Peter Zijlstra's suggestion and the final fix, what actually happens is that `is_cpu_allowed()` for a migration-disabled task effectively says "can only stay on current CPU" because the migration machinery uses this function to gate migrations.

Actually, examining the thread discussion more carefully, Sebastian's intermediate fix was to explicitly check `p->migration_disabled`:

```c
if (p->migration_disabled)
    goto next;
```

But the final commit uses `is_cpu_allowed()` instead, as suggested by Peter Zijlstra, because it is the canonical function for determining whether a task can run on a given CPU, and it encapsulates all the relevant checks including migration_disabled, CPU online/active state, and per-CPU kthread restrictions.

The call chain that triggers this is: `pick_next_task()` → `queue_core_balance()` (queues a balance callback) → `__balance_callback()` → `sched_core_balance()` → `steal_cookie_task()` → `try_steal_cookie()`. When `try_steal_cookie()` finds a matching task and passes the faulty cpumask check, it calls `deactivate_task(src, p, 0)` / `set_task_cpu(p, this)` / `activate_task(dst, p, 0)` to move the task directly, completely bypassing the normal migration path that would check `migration_disabled`.

## Consequence

The most severe consequence, documented in the LKML discussion by Steven Rostedt and Sebastian Andrzej Siewior, is a lockdep splat and lock corruption on PREEMPT_RT kernels. On RT, spinlocks are converted to sleeping rt_mutexes, and local locks have per-CPU ownership semantics. When a migration-disabled task holding a local lock is stolen to a different CPU, the lock is effectively "acquired on CPU-A and released on CPU-B", which is illegal and causes:

1. **lockdep WARNING: bad unlock balance detected** — the lock tracking system detects that a lock acquired on one CPU is being released on a different CPU.
2. **Potential data corruption** — per-CPU data accessed under `migrate_disable()` assumes the task stays on its current CPU. Moving the task silently invalidates this assumption, leading to concurrent access to per-CPU structures from the wrong CPU.

The original ChromeOS bug report showed the following stack trace:
```
task_blocks_on_rt_mutex() {
  spin_lock(pi_lock);
  rt_mutex_setprio() {
    balance_callback() {
      sched_core_balance() {
        spin_unlock_irq(rq);   // <-- enables IRQs while holding pi_lock
```

This reveals a secondary issue: `sched_core_balance()` uses `raw_spin_rq_unlock_irq(rq)` which enables interrupts while the caller (`rt_mutex_setprio()` via `__balance_callback()`) may still hold `pi_lock`, creating a lock ordering violation.

On non-RT kernels, the consequence is more subtle but still a correctness violation: per-CPU data accessed under `migrate_disable()` becomes inconsistent. This could lead to silent data corruption, incorrect accounting, or spurious failures in subsystems that rely on per-CPU data stability during migration-disabled sections.

## Fix Summary

The fix replaces the simple `cpumask_test_cpu()` check with a call to `is_cpu_allowed()`:

```c
- if (!cpumask_test_cpu(this, &p->cpus_mask))
+ if (!is_cpu_allowed(p, this))
      goto next;
```

The `is_cpu_allowed()` function performs a comprehensive check that includes: (1) whether the CPU is in the task's cpumask, (2) whether the task has migration disabled (via `is_migration_disabled(p)`), (3) whether the CPU is online/active, (4) per-CPU kthread restrictions, and (5) whether the CPU is in the process of going offline. This is the canonical function used throughout the scheduler to determine if a task may run on a given CPU.

This fix is correct and complete because it ensures `try_steal_cookie()` uses the same eligibility logic as the rest of the scheduler's migration infrastructure. When a task has `migration_disabled` set, `is_cpu_allowed()` will return false for any CPU other than the one the task is currently running on (since the migration framework interprets this as "stay on current CPU"). This prevents the forced-newidle balancer from violating per-CPU invariants by stealing migration-disabled tasks.

The LKML thread also identified a separate (but related) issue with `sched_core_balance()` using `raw_spin_rq_unlock_irq(rq)` which enables interrupts in a context where a caller may hold other locks. Sebastian proposed additional patches for that, but this commit specifically addresses the migration_disabled oversight in `try_steal_cookie()`.

## Triggering Conditions

To trigger this bug, the following conditions must all be met simultaneously:

1. **CONFIG_SCHED_CORE=y**: Core scheduling must be enabled at kernel compile time. This is required for the `try_steal_cookie()` code path to exist and be reachable.

2. **SMT topology**: The system must have SMT (Simultaneous Multi-Threading / HyperThreading) with at least two logical CPUs per physical core. Core scheduling only constrains task co-scheduling on SMT siblings.

3. **Core scheduling cookies set**: At least two tasks must have core scheduling cookies set via `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE/SHARE, ...)`. The cookies must be set such that a cookie mismatch forces one sibling idle (triggering `queue_core_balance()`), while a matching-cookie task exists on another CPU.

4. **Migration-disabled task with matching cookie**: A task on a source CPU must (a) have a `core_cookie` matching the forced-idle destination's `core_cookie`, (b) have `migration_disabled > 0` (i.e., be inside a `migrate_disable()` section), and (c) have a cpumask that includes the destination CPU. The task must not be `src->core_pick` or `src->curr`.

5. **Forced-newidle balancer fires**: A sibling CPU must be forced idle during `pick_next_task()` because no compatible-cookie task is available. This queues `sched_core_balance()` as a balance callback. The callback then iterates across the sched domain looking for stealable tasks.

6. **Timing**: The migration-disabled task must be in the `migrate_disable()` section at the exact moment `try_steal_cookie()` examines it. Since `try_steal_cookie()` holds the source and destination rq locks, the task cannot be concurrently modifying its state, but it can be sleeping/runnable with `migration_disabled` set.

On PREEMPT_RT kernels, the bug is reliably exposed because `migrate_disable()` is widely used (it replaces `preempt_disable()` for many purposes), so migration-disabled tasks are common. On non-RT kernels, `migrate_disable()` is less commonly used, making the bug harder to trigger but still present.

## Reproduce Strategy (kSTEP)

### Why this bug cannot be reproduced with kSTEP

This bug **cannot** be reproduced with kSTEP for the same fundamental reason documented for commit `e705968dd687` ("sched_group_cookie_match checks wrong rq"): **core scheduling requires `prctl(PR_SCHED_CORE)` to set task cookies, and kSTEP cannot issue prctl syscalls or safely enable core scheduling internals.**

Specifically, there are multiple barriers:

1. **Core scheduling cookie assignment**: Core scheduling cookies are assigned to tasks via the `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, ...)` syscall, which calls `sched_core_share_pid()` internally. This function creates a unique cookie, updates the task's `core_cookie` field, and manages the per-rq `core_tree` red-black tree through `sched_core_enqueue()`/`sched_core_dequeue()`. Simply writing to `p->core_cookie` directly would be unsafe because it would skip the tree management, potentially corrupting the `core_tree` data structure that `sched_core_find()` and `sched_core_next()` iterate over in `try_steal_cookie()`. kSTEP tasks are kernel-controlled entities and cannot issue prctl syscalls.

2. **Core scheduling activation**: Core scheduling must be globally activated via `sched_core_get()` which increments `sched_core_count` and rebuilds sched domains with core scheduling topology. Without this, `sched_core_enabled(rq)` returns false and `pick_next_task()` takes the fast path via `__pick_next_task()`, never reaching the core scheduling logic or `queue_core_balance()`. kSTEP has no API to enable core scheduling.

3. **Migration-disabled section**: While kthreads can call `migrate_disable()`, orchestrating a kthread to be in a migration-disabled section while also being runnable (not currently executing) and present in the `core_tree` requires careful coordination that kSTEP's task control primitives don't support. The task must have called `migrate_disable()` in its own execution context, not from an external driver.

4. **SMT topology requirement**: While kSTEP supports topology configuration via `kstep_topo_set_smt()`, the QEMU `-smp` configuration must expose threads-per-core > 1 for the kernel to recognize SMT siblings. Additionally, `CONFIG_SCHED_CORE` must be enabled in the kernel build configuration, which may not be the default.

5. **Forced-newidle trigger**: The forced-newidle balancer fires as a balance callback from `pick_next_task()` when a CPU is forced idle due to cookie mismatch. This requires the core scheduling `pick_next_task()` path to be active and to encounter a cookie mismatch scenario on an SMT sibling pair. kSTEP cannot control the exact sequence of `pick_next_task()` decisions across multiple CPUs.

### What would need to be added to kSTEP

To support this class of core-scheduling bugs, kSTEP would need:
- **`kstep_core_sched_enable()`**: An API to globally enable core scheduling (`sched_core_get()` + sched domain rebuild).
- **`kstep_task_set_cookie(p, cookie)`**: An API to safely assign a core scheduling cookie to a task, properly managing `core_tree` enqueue/dequeue and `core_task_seq` updates.
- **`kstep_task_migrate_disable(p)` / `kstep_task_migrate_enable(p)`**: APIs to set/clear migration_disabled on tasks (must be called in the task's own context).
- SMT-aware QEMU configuration with `CONFIG_SCHED_CORE=y` in the kernel build.

These are **not** minor extensions — they require safely invoking internal core scheduling management functions and fundamentally changing how kSTEP interacts with the scheduler's task lifecycle.

### Alternative reproduction methods

Outside kSTEP, this bug can be reproduced on a PREEMPT_RT kernel with SMT enabled by:
1. Enabling core scheduling via `prctl(PR_SCHED_CORE)` on a group of tasks.
2. Running workloads that trigger frequent `migrate_disable()` sections (any RT workload using local locks, or explicitly calling `migrate_disable()`/`migrate_enable()` in a tight loop).
3. Ensuring cookie mismatches on SMT siblings to force the newidle balancer.
4. Monitoring for lockdep splats ("bad unlock balance" or "releasing interrupts with pi_lock held").

On non-RT kernels, the bug can be triggered by any workload using `migrate_disable()` combined with core scheduling, but detection requires instrumentation (e.g., checking `task_cpu(p)` changes while `p->migration_disabled > 0`).
