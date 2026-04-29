# Core: affine_move_task() stopper list corruption from concurrent set_cpus_allowed_ptr()

**Commit:** `9e81889c7648d48dd5fe13f41cbc99f3c362484a`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.12-rc3
**Buggy since:** v5.11-rc1 (introduced by `6d337eab041d` "sched: Fix migrate_disable() vs set_cpus_allowed_ptr()")

## Bug Description

When two threads concurrently call `sched_setaffinity()` (or `set_cpus_allowed_ptr()`) on the same target task, the internal `set_affinity_pending` structure's `stop_work` field can be queued twice onto the CPU stopper's work list. This happens because both callers discover the same `p->migration_pending` pointer installed by the first caller, and both independently issue `stop_one_cpu_nowait()` using the same `pending->stop_work`. Since `cpu_stop_work` uses a linked list node (`list_head`), queuing the same node twice corrupts the stopper's work list.

The bug was introduced in commit `6d337eab041d` ("sched: Fix migrate_disable() vs set_cpus_allowed_ptr()"), which reworked the affinity migration path to use `set_affinity_pending` with asynchronous stopper work via `stop_one_cpu_nowait()`. Prior to that commit, `affine_move_task()` used synchronous `stop_one_cpu()` in the non-MIGRATE_ENABLE path, which naturally serialized the stopper invocations. The conversion to asynchronous stoppers opened a window where a second caller could fire a duplicate stopper for the same pending structure.

This is part of a 6-patch series by Peter Zijlstra fixing multiple issues in the `affine_move_task()` / `migration_cpu_stop()` interaction. This specific patch (5/6) addresses the self-concurrency problem, while companion patches fix related issues like uninitialized `pending->arg` (patch 1/6), simplification of the stopper flow (patch 2/6), collapsing duplicate code paths (patch 3/6), and refcount simplification (patch 6/6).

## Root Cause

The root cause is a missing guard against duplicate stopper submissions for the same `set_affinity_pending` structure. In `affine_move_task()`, when the target task is running (`task_running(rq, p)`) or waking (`p->state == TASK_WAKING`), the function cannot directly migrate the task and must delegate to a CPU stopper. The relevant code path in the buggy kernel is:

```c
if (task_running(rq, p) || p->state == TASK_WAKING) {
    refcount_inc(&pending->refs); /* pending->{arg,stop_work} */
    if (flags & SCA_MIGRATE_ENABLE)
        p->migration_flags &= ~MDF_PUSH;
    task_rq_unlock(rq, p, rf);

    stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop,
                        &pending->arg, &pending->stop_work);
    ...
}
```

The sequence of events for the bug:

1. **Thread A** calls `set_cpus_allowed_ptr(p, X)`. In `affine_move_task()`, since `p->migration_pending` is NULL, Thread A allocates `my_pending` on its stack and sets `p->migration_pending = &my_pending`. The target task `p` is running, so Thread A enters the `task_running()` branch. It reads `pending = p->migration_pending` (which is `&my_pending`), increments `pending->refs`, releases `task_rq_unlock()` (which drops `p->pi_lock` and `rq->lock`), and calls `stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop, &pending->arg, &pending->stop_work)`. This enqueues `pending->stop_work` onto the target CPU's stopper list.

2. **Thread B** calls `set_cpus_allowed_ptr(p, Y)` before the stopper from step 1 has executed. In `affine_move_task()`, Thread B acquires `p->pi_lock` and `rq->lock`, finds `p->migration_pending` is non-NULL (still pointing to Thread A's `my_pending`), so it takes the `else` branch and increments `pending->refs` (on the same `pending`). The target task is still running, so Thread B also enters the `task_running()` branch. Thread B reads the same `pending`, increments `pending->refs` again, releases the locks, and calls `stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop, &pending->arg, &pending->stop_work)` — with the **exact same** `&pending->stop_work`.

3. The second `stop_one_cpu_nowait()` attempts to add `pending->stop_work` to the stopper's work list again, but it is already on the list. This corrupts the linked list because `list_add_tail()` on an already-enqueued node creates a cycle or dangling pointers in the list.

The critical invariant that was violated is: a given `cpu_stop_work` instance must only be queued once at a time. The `stop_one_cpu_nowait()` function does not check for duplicates; it unconditionally adds the work to the stopper's list.

## Consequence

The immediate consequence is **stopper list corruption**. The CPU stopper thread's work list (`struct cpu_stopper::works`) is a doubly-linked list, and queuing the same `list_head` node twice causes the list to enter an inconsistent state. This can manifest in several ways:

- **Kernel crash / BUG**: When the stopper thread processes its work list, it may encounter a corrupted `list_head` (e.g., a node whose `prev`/`next` pointers form a cycle or point to freed memory), leading to a NULL pointer dereference, use-after-free, or a `list_debug` BUG assertion if `CONFIG_DEBUG_LIST` is enabled.
- **Infinite loop in stopper thread**: A corrupted list can cause the stopper thread to loop forever when traversing its work list, effectively deadlocking the CPU since the stopper is a high-priority kernel thread (SCHED_FIFO, max priority). This would prevent any further stop_machine operations, CPU hotplug, or task migrations on that CPU.
- **Use-after-free**: Since `my_pending` is allocated on Thread A's stack, once Thread A's `wait_for_completion()` returns and the function exits, the `stop_work` node (still linked in the stopper list) points to freed stack memory. When the stopper thread later dereferences it, this is a use-after-free that could corrupt arbitrary kernel memory or crash.

The bug is most likely to manifest under moderate to heavy concurrent affinity changes on the same task, such as workloads using `taskset`, container runtimes adjusting CPU affinity, or load balancers that frequently repin tasks.

## Fix Summary

The fix adds a new field `unsigned int stop_pending` to `struct set_affinity_pending` that tracks whether a stopper work is already in-flight for this pending. Before issuing `stop_one_cpu_nowait()`, the code now checks `pending->stop_pending`:

```c
stop_pending = pending->stop_pending;
if (!stop_pending)
    pending->stop_pending = true;
...
if (!stop_pending) {
    stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop,
                        &pending->arg, &pending->stop_work);
}
```

This ensures only the **first** caller through this path actually enqueues the stopper work. Subsequent callers that find the same `pending` skip the `stop_one_cpu_nowait()` call, since a stopper is already queued that will handle the migration. The check and set of `stop_pending` is done under `p->pi_lock` (held via `task_rq_lock()`), so it is properly serialized.

In `migration_cpu_stop()`, the stopper callback, two additional changes are made: (1) A `WARN_ON_ONCE(!pending->stop_pending)` is added in the re-queue path to catch violations of the invariant, and (2) `pending->stop_pending = false` is set in the `out:` path when the stopper completes, allowing a future stopper to be queued if needed. The re-queue path in `migration_cpu_stop()` (when the task has moved to another CPU and still needs migration) inherently has `stop_pending == true` since it only runs from an active stopper, so it correctly sets up a new stopper without corruption.

## Triggering Conditions

The bug requires the following precise conditions:

- **Two concurrent callers** of `set_cpus_allowed_ptr()` (or `sched_setaffinity()`) targeting the **same task** `p`. The callers must be on different CPUs or be preemptible so they can actually run concurrently.
- **Target task must be running** on a CPU (`task_running(rq, p)` returns true) or in the `TASK_WAKING` state when both callers process it in `affine_move_task()`. If the task is sleeping and queued, it would be directly migrated via `move_queued_task()` without needing a stopper.
- **The first caller's stopper must not have executed yet** when the second caller reaches `affine_move_task()`. This is a timing window: after the first caller releases `pi_lock` and before the stopper thread runs. On a busy system or a system with preemption, this window can be significant.
- **The new CPU mask must not include the task's current CPU** for both callers, so that actual migration is required (otherwise the early-return "task is already on a valid CPU" path is taken).
- **At least 2 CPUs** are required. The stopper list corruption occurs on the CPU running the target task.
- **Kernel version**: v5.11-rc1 through v5.12-rc2 (commits containing `6d337eab041d` but not the fix `9e81889c7648`).

The race is not extremely unlikely under real workloads — container orchestrators, cgroup cpuset adjustments, and userspace tools like `taskset` can trigger concurrent affinity changes. A `!PREEMPT` kernel makes the window larger (as noted in the commit message comments about `migration_cpu_stop()`).

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The primary reason is that the fix targets kernel version v5.12-rc3, and the bug exists only in kernels from v5.11-rc1 to v5.12-rc2. kSTEP supports Linux v5.15 and newer only. The buggy code was already fixed well before v5.15, so kSTEP cannot check out a kernel version that contains the bug.

2. **WHAT would need to be added to kSTEP to support this?**
   Even if the kernel version were supported, reproducing this bug with kSTEP would require the ability to trigger concurrent `set_cpus_allowed_ptr()` calls from two separate execution contexts (e.g., two kthreads) on the same target task. kSTEP's `kstep_task_pin()` API does call `set_cpus_allowed_ptr()`, but it would need to be invoked concurrently from two kthreads with precise timing to hit the race window. This is conceptually possible with kSTEP's kthread support (`kstep_kthread_create`, `kstep_kthread_start`), but the kernel version constraint is the fundamental blocker.

   If the version requirement were lifted, the approach would be:
   - Create a target CFS task pinned to CPU 1
   - Create two kthreads on different CPUs that both call `set_cpus_allowed_ptr()` on the target task with different masks (e.g., one changes to CPU 2, the other to CPU 3)
   - Synchronize the two kthreads to call `set_cpus_allowed_ptr()` as close to simultaneously as possible
   - Detect corruption via `CONFIG_DEBUG_LIST` assertions, kernel panics, or by checking the stopper thread's work list integrity

3. **Version constraint**: The fix is in v5.12-rc3, which is before the v5.15 minimum supported by kSTEP. This is the definitive reason for placing this in `drivers_unplanned`.

4. **Alternative reproduction methods outside kSTEP**:
   - Boot a v5.11 or v5.12-rc1/rc2 kernel in QEMU
   - Write a userspace program that spawns two threads, each calling `sched_setaffinity()` on a third (busy-looping) thread in a tight loop with alternating CPU masks
   - Enable `CONFIG_DEBUG_LIST` to get early detection of list corruption
   - Run on a multi-core system (2+ CPUs) with the target thread bound to a specific CPU initially
   - The race should trigger relatively quickly under high concurrency, especially on `!PREEMPT` kernels where the stopper execution window is wider
   - Example: Thread T runs on CPU 0; Thread A calls `sched_setaffinity(T, {1})` repeatedly while Thread B calls `sched_setaffinity(T, {2})` repeatedly. The stopper list corruption should manifest as a kernel crash, list debug BUG, or hang within seconds to minutes.
