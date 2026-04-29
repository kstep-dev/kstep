# Core: migration_cpu_stop() NULL task pointer from uninitialized pending->arg

**Commit:** `8a6edb5257e2a84720fe78cb179eca58ba76126f`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.12-rc3
**Buggy since:** v5.10-rc1 (commit `6d337eab041d` — "sched: Fix migrate_disable() vs set_cpus_allowed_ptr()")

## Bug Description

When `set_cpus_allowed_ptr()` is called on a running task to change its CPU affinity, the scheduler invokes `affine_move_task()` to handle the migration. For a running task that does not already have a pending affinity change, `affine_move_task()` sets up `p->migration_pending` (a `struct set_affinity_pending`) and then uses `stop_one_cpu()` to schedule `migration_cpu_stop()` on the CPU hosting the target task. The `migration_cpu_stop()` function, executing as a high-priority stopper thread on the target CPU, is supposed to bump the task off the CPU and migrate it to the new destination.

However, a race condition can arise: between the time `affine_move_task()` schedules the stopper work and the time `migration_cpu_stop()` executes, the target task may have already been migrated away by a concurrent load-balance operation or other migration mechanism. In this case, `migration_cpu_stop()` finds that `task_rq(p) != rq` (the task is no longer on the current CPU's runqueue), and it falls into the else-if branch `} else if (dest_cpu < 1 || pending)`. Since `p->migration_pending` was set earlier, `pending` is non-NULL, and this branch is taken.

The else-if branch attempts to re-issue `migration_cpu_stop()` on the CPU now hosting the target task, using `pending->arg` as the argument. The critical problem is that `pending->arg` was never initialized. The `affine_move_task()` function used a local `struct migration_arg arg` variable (initialized with `.task = p` and `.dest_cpu = dest_cpu`) when calling `stop_one_cpu()`, but the `pending->arg` embedded inside `struct set_affinity_pending` remained zero-initialized. When `migration_cpu_stop()` re-queues itself using `pending->arg`, the next invocation receives an argument with `arg->task == NULL`, leading to a NULL pointer dereference.

## Root Cause

The root cause is a disconnect between two data structures used to pass arguments to `migration_cpu_stop()`. In the buggy code path within `affine_move_task()`, when a non-`SCA_MIGRATE_ENABLE` affinity change targets a running task (or a task in `TASK_WAKING` state), the function enters this branch:

```c
if (task_running(rq, p) || p->state == TASK_WAKING) {
    task_rq_unlock(rq, p, rf);
    stop_one_cpu(cpu_of(rq), migration_cpu_stop, &arg);
}
```

Here, `arg` is a stack-local `struct migration_arg` containing `.task = p` and `.dest_cpu = dest_cpu`. This works fine for the first invocation of `migration_cpu_stop()`. However, the `struct set_affinity_pending my_pending` that was installed on `p->migration_pending` has its own embedded `arg` field (`my_pending.arg`) which is zero-initialized by the `= { }` aggregate initialization and never explicitly set.

Meanwhile, the `SCA_MIGRATE_ENABLE` code path correctly initialized `pending->arg` before using it:

```c
pending->arg = (struct migration_arg) {
    .task = p,
    .dest_cpu = -1,
    .pending = pending,
};
stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop,
                    &pending->arg, &pending->stop_work);
```

But the non-`SCA_MIGRATE_ENABLE` path for running tasks used the local `arg` instead, leaving `pending->arg` uninitialized.

When `migration_cpu_stop()` detects the task has moved away (task_rq(p) != rq) and needs to re-issue itself on the task's new CPU, it executes:

```c
stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop,
                    &pending->arg, &pending->stop_work);
```

Since `pending->arg` is all zeros, the re-issued `migration_cpu_stop()` dereferences `arg->task` which is `NULL`, causing a NULL pointer dereference kernel crash.

Additionally, the buggy code also had the problem that `migration_cpu_stop()` would pick up `p->migration_pending` unconditionally, even when called from `sched_exec()` or `migrate_task_to()` (which use their own local `arg` without any pending). This meant that `migration_cpu_stop()` could interfere with pending state from `affine_move_task()` calls that had their own stop_work in flight to manage the completion.

## Consequence

The immediate consequence is a **NULL pointer dereference kernel crash** (oops/panic). When `migration_cpu_stop()` runs with the uninitialized `pending->arg` argument, it attempts to dereference `arg->task` (which is `NULL`) in multiple places — for example `raw_spin_lock(&p->pi_lock)` where `p` is `NULL`, or in `task_rq(p)` which accesses `p->on_cpu`. This results in a kernel oops that typically manifests as a "BUG: unable to handle page fault" at a very low address (near address 0).

The crash occurs in the stopper thread context, which is a high-priority kernel thread. A crash here can take down the entire system. As Peter Zijlstra noted in the original RFC, the crash is described as "fireworks" — the NULL dereference is obvious and immediately fatal. The original reporter confirmed the crash occurred in production, though Zijlstra noted that reproducing it required removing an early termination condition `[A]` (the check `cpumask_test_cpu(task_cpu(p), p->cpus_ptr)`) which narrowed the race window considerably.

The scenario requires concurrent task migration (e.g., from load balancing) happening at exactly the right moment relative to the affinity change, making this a rare but catastrophic race condition. When it does trigger, there is no recovery — it is a hard kernel crash.

## Fix Summary

The fix restructures `affine_move_task()` to always use `pending->arg` as the migration argument, rather than a stack-local `struct migration_arg`. Specifically:

1. **Initialize `pending->arg` at installation time**: When a new `my_pending` is created and installed on `p->migration_pending`, the fix immediately initializes `my_pending.arg` with the proper values (`.task = p`, `.dest_cpu = -1` meaning "any valid CPU", `.pending = &my_pending`). This ensures `pending->arg` is always valid whenever `pending` is referenced.

2. **Replace `stop_one_cpu()` with `stop_one_cpu_nowait()` for running tasks**: The non-`SCA_MIGRATE_ENABLE` running-task case now uses `stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop, &pending->arg, &pending->stop_work)` instead of `stop_one_cpu(cpu_of(rq), migration_cpu_stop, &arg)`. This aligns it with the `SCA_MIGRATE_ENABLE` path and ensures that the same `pending->arg` pointer is used both for the initial invocation and any subsequent re-queuing by `migration_cpu_stop()`.

3. **Add `arg->pending` check in `migration_cpu_stop()`**: A new check at the top of `migration_cpu_stop()` distinguishes between the two use cases: calls from `sched_exec()`/`migrate_task_to()` (where `arg->pending == NULL`) and calls from `affine_move_task()` (where `arg->pending != NULL`). When `migration_cpu_stop()` sees `pending && !arg->pending`, it clears `pending` to `NULL`, preventing interference between the two independent migration mechanisms. This creates a clean separation: only `affine_move_task()`-originated stopper work handles `migration_pending` completions.

These three changes together ensure that `pending->arg` is always properly initialized before it can be used, that the same argument structure is used consistently across re-queuing, and that unrelated stopper invocations don't accidentally interact with the pending affinity machinery.

## Triggering Conditions

The bug requires the following specific conditions to occur simultaneously:

- **Two or more CPUs**: The target task must be running on one CPU while `set_cpus_allowed_ptr()` is called from another CPU (or context). The re-queuing path targets the task's new CPU, so at minimum 2 CPUs are needed.

- **A running task receiving an affinity change**: `set_cpus_allowed_ptr()` must be called with a new CPU mask on a task that is currently `task_running()` (actively on-CPU) or in `TASK_WAKING` state. This enters the `stop_one_cpu()` path in `affine_move_task()` where the local `arg` is used but `pending->arg` is not initialized.

- **No pre-existing `migration_pending`**: The task must not already have a `migration_pending` set from a prior affinity change. If it did, the code would take the `refcount_inc` path and reuse the existing pending, which may already have its `arg` initialized (from the `SCA_MIGRATE_ENABLE` path).

- **Concurrent task migration**: Between `affine_move_task()` scheduling the stopper work and `migration_cpu_stop()` executing, the target task must be migrated to a different CPU by another mechanism (e.g., load balancing, another `set_cpus_allowed_ptr()`, or `sched_exec()`). This causes `task_rq(p) != rq` in `migration_cpu_stop()`.

- **Task not yet on a valid CPU**: After the concurrent migration, the task must NOT be on a CPU that satisfies the new affinity mask. If it is, the early check `cpumask_test_cpu(task_cpu(p), p->cpus_ptr)` returns true and the function bails out before reaching the re-queuing code. Peter Zijlstra noted that this early check makes the window extremely narrow in practice — he could only reproduce the crash after removing this check.

- **Kernel version v5.10-rc1 through v5.12-rc2**: The bug was introduced by commit `6d337eab041d` in v5.10-rc1 and fixed in v5.12-rc3. The race is architecture- and workload-independent but extremely timing-sensitive.

## Reproduce Strategy (kSTEP)

### Why this bug cannot be reproduced with kSTEP

**The kernel version is too old.** The bug was introduced in commit `6d337eab041d` (merged in v5.10-rc1) and fixed in commit `8a6edb5257e2a84720fe78cb179eca58ba76126f` (merged in v5.12-rc3). kSTEP supports Linux v5.15 and newer only. By the time v5.15 was released, this fix had been present for over a year. There is no supported kernel version within kSTEP's range that contains the buggy code.

### What would be needed to reproduce this in kSTEP

If kSTEP supported older kernels (v5.10–v5.12), reproducing this bug would require:

1. **Triggering a `set_cpus_allowed_ptr()` call on a running task**: kSTEP provides `kstep_task_pin(p, begin, end)` which calls `set_cpus_allowed_ptr()`. This would need to be called on a task that is actively running on a CPU.

2. **Causing a concurrent migration**: The task must be migrated away by load balancing or another mechanism between the `affine_move_task()` call and the `migration_cpu_stop()` execution. kSTEP could potentially trigger this with `kstep_task_pin()` from two different contexts on different CPUs simultaneously, or by relying on load balancer migration. The `on_sched_balance_begin` callback could help coordinate this.

3. **Narrowing the race window**: As Zijlstra noted, the race is nearly impossible to hit without removing the early `cpumask_test_cpu()` check in `migration_cpu_stop()`. Even with kSTEP's tick control and task management, the stopper thread scheduling is outside kSTEP's direct control, making precise timing of the race extremely difficult.

4. **Detection**: The bug manifests as a NULL pointer dereference, which would cause a kernel oops/panic. This would be detectable as a kernel crash in the QEMU guest, terminating the test run. kSTEP could check for kernel crash messages in `dmesg` or simply detect that the guest became unresponsive.

### Alternative reproduction methods

Outside kSTEP, reproduction could be attempted by:

- Running a workload that performs frequent `sched_setaffinity()` syscalls on running tasks while also triggering heavy load balancing across CPUs (e.g., many tasks with varying CPU-intensive workloads).
- As Zijlstra suggested, removing the early termination check `[A]` (`cpumask_test_cpu(task_cpu(p), p->cpus_ptr)`) from `migration_cpu_stop()` dramatically widens the race window and allows reliable reproduction. This would require a custom kernel build.
- Using stress-ng or similar tools to create high concurrency between affinity changes and load balancing on a multi-CPU system running a kernel between v5.10 and v5.12-rc2.
