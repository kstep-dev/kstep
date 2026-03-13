# Core: ttwu() race due to stale task_cpu() load ordering

**Commit:** `b6e13e85829f032411b896bd2f0d6cbe4b0a3c4a`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.8-rc3
**Buggy since:** v5.8-rc1 (introduced by commit c6e7bd7afaeb "sched/core: Optimize ttwu() spinning on p->on_cpu")

## Bug Description

A race condition in `try_to_wake_up()` (ttwu) allows a stale value of `task_cpu(p)` to be used when deciding whether to offload a wakeup to a remote CPU's wake_list. The bug was introduced in the v5.8-rc1 merge window by commit `c6e7bd7afaeb` ("sched/core: Optimize ttwu() spinning on p->on_cpu"), which added an optimization to avoid spinning on `p->on_cpu` by instead queueing the wakeup on the CPU that owns the `p->on_cpu` value. However, the code read `cpu = task_cpu(p)` at the beginning of `try_to_wake_up()`, before the `p->on_cpu` load that guards the wakelist path, creating a window where the CPU value could be stale.

The bug was reported by Paul E. McKenney after rcutorture occasionally hit a NULL pointer dereference. The crash occurred in the CFS wakeup preemption path: `sched_ttwu_pending()` → `ttwu_do_wakeup()` → `check_preempt_curr()` → `check_preempt_wakeup()` → `find_matching_se()` → `is_same_group()`, where `se->cfs_rq == pse->cfs_rq` dereferenced a NULL pointer. This happened because the task was enqueued on a CPU's runqueue using a `cfs_rq` from the wrong CPU, leading to iteration into non-existent parent scheduling entities.

The race requires a specific interleaving across at least three CPUs involving task migration, context switching, sleeping, and a concurrent wakeup. Peter Zijlstra noted that reproduction required many CPU hours of rcutorture and that most previous ttwu() races were found on very large PowerPC systems with weaker memory ordering guarantees. On x86_64, where every LOCK implies a full memory barrier, the race is much harder to trigger.

## Root Cause

The root cause is a missing memory ordering constraint between reading `task_cpu(p)` and reading `p->on_cpu` in `try_to_wake_up()`. Before the fix, the code did:

```c
cpu = task_cpu(p);    // Read p->cpu early
// ... later ...
smp_rmb();
if (READ_ONCE(p->on_cpu) && ttwu_queue_wakelist(p, cpu, wake_flags | WF_ON_RQ))
    goto unlock;
```

The `cpu` variable was loaded at the top of `try_to_wake_up()`, before the `p->on_cpu` check. On weakly-ordered architectures (like PowerPC), the processor could reorder these loads such that `p->on_cpu` is loaded (seeing 1, meaning the task is currently running on some CPU) while `task_cpu(p)` returns a stale value from before a recent migration.

The elaborate race scenario described by Peter Zijlstra involves three CPUs and task X:

1. **CPU1** switches away from task X: dequeues X, context-switches to Z, clears `X->on_cpu = 0`, unlocks `rq(1)->lock`.
2. **CPU2** migrates X to CPU0: locks `rq(1)`, dequeues X, calls `set_task_cpu(X, 0)` setting `X->cpu = 0`, unlocks `rq(1)`, then locks `rq(0)`, enqueues X with `X->on_rq = 1`, unlocks `rq(0)`.
3. **CPU0** switches to X: locks `rq(0)`, context-switches to X setting `X->on_cpu = 1`, unlocks `rq(0)`.
4. **CPU0** (as X): X sets `X->state = TASK_UNINTERRUPTIBLE` and issues `smp_mb()`.
5. **CPU1** concurrently calls `ttwu()` to wake X: acquires `X->pi_lock`, checks `p->state` (sees UNINTERRUPTIBLE), reads `cpu = X->cpu` — but due to weak memory ordering, sees the **stale** value of 1 (CPU1) instead of the current value of 0 (CPU0).
6. **CPU0** (as X): X calls `schedule()`, dequeues itself (`X->on_rq = 0`), context-switches to Y, clears `X->on_cpu = 0`.
7. **CPU1** (in ttwu): reads `p->on_rq` (sees 0), reads `p->on_cpu` (sees 1 — X is still running on CPU0 at this moment), and calls `ttwu_queue_wakelist(p, cpu=1, ...)` — queueing the wakeup on CPU1's wake_list with `cpu == smp_processor_id()`.

The critical issue is at step 7: `cpu == smp_processor_id()` (both are 1), which should be impossible because `p->on_cpu` can only be true for remote tasks. The task gets enqueued locally on CPU1, but its `se.cfs_rq` still points to CPU0's CFS runqueue. When `check_preempt_wakeup()` calls `find_matching_se()` → `is_same_group()`, it compares `se->cfs_rq` (CPU0's) with `pse->cfs_rq` (CPU1's). Since they differ, the code iterates up the parent chain looking for a common ancestor, eventually hitting a NULL pointer and crashing.

The fundamental ordering problem is: `set_task_cpu(p, cpu)` stores `p->cpu = @cpu` *before* `__schedule()` stores `p->on_cpu = 1` (under the rq lock). Without an acquire barrier on the `p->on_cpu` load that pairs with the release in the `LOCK rq->lock` + `smp_mb__after_spinlock()` sequence, the `p->cpu` load can be reordered before `p->on_cpu` and return a stale value.

## Consequence

The primary observable consequence is a **NULL pointer dereference** in the kernel, resulting in a kernel oops or panic. The crash occurs in the CFS scheduling path during wakeup preemption checking:

```
sched_ttwu_pending()
  ttwu_do_wakeup()
    check_preempt_curr() := check_preempt_wakeup()
      find_matching_se()
        is_same_group()
          if (se->cfs_rq == pse->cfs_rq) <-- NULL deref
```

This happens because the woken task is enqueued on a runqueue whose CFS hierarchy does not match the task's scheduling entity's `cfs_rq` pointer. When traversing up the sched_entity parent chain to find a common group, the code walks past the root of the hierarchy and dereferences a NULL parent pointer.

Without the `ttwu_queue_wakelist()` optimization (i.e., on the older code path), the same stale CPU scenario would instead cause an **instant live-lock** in `smp_cond_load_acquire(&p->on_cpu, !VAL)`, spinning forever with IRQs disabled on the local CPU because `p->on_cpu` would never be cleared locally (the task is running on a different CPU). However, this live-lock variant was never reported, possibly because the optimization in `c6e7bd7afaeb` was needed to expose the race in practice.

The bug is a reliability and stability issue. It was discovered through extensive rcutorture stress testing, taking many CPU hours to reproduce. It primarily affects systems with weak memory ordering (e.g., large PowerPC systems), though the theoretical race exists on all SMP architectures.

## Fix Summary

The fix reorders the `task_cpu(p)` load to occur *after* the `p->on_cpu` load, and upgrades the `p->on_cpu` load from `READ_ONCE()` to `smp_load_acquire()`. This establishes a proper ordering: the acquire semantics on `p->on_cpu` pair with the `LOCK rq->lock` + `smp_mb__after_spinlock()` sequence in `__schedule()`, which itself orders after the `STORE p->cpu = @cpu` in `set_task_cpu()`. Thus, if ttwu observes `p->on_cpu == 1`, it is guaranteed to also observe the correct (current) value of `p->cpu`.

Specifically, the change replaces:
```c
cpu = task_cpu(p);   // too early
// ...
if (READ_ONCE(p->on_cpu) && ttwu_queue_wakelist(p, cpu, ...))
```
with:
```c
// cpu is NOT loaded here
// ...
if (smp_load_acquire(&p->on_cpu) &&
    ttwu_queue_wakelist(p, task_cpu(p), ...))  // task_cpu(p) loaded AFTER on_cpu
```

The `cpu` variable is removed entirely from the early part of `try_to_wake_up()` (both the `p == current` path and the general SMP path). On the SMP path, `task_cpu(p)` is now read inline at the point of use after the `smp_load_acquire(&p->on_cpu)`, or after `smp_cond_load_acquire()` + `select_task_rq()`. On the non-SMP path, `cpu = task_cpu(p)` is deferred to just before `ttwu_queue()`. The `ttwu_stat()` call at the end also uses `task_cpu(p)` instead of the `cpu` local variable.

Additionally, the fix adds two defensive WARN_ON_ONCE checks in `sched_ttwu_pending()` to detect if the race occurs despite the fix: one warns if `p->on_cpu` is still set when processing the wake_list, and another warns if `task_cpu(p)` does not match the current CPU. A third WARN_ON_ONCE in `ttwu_queue_wakelist()` catches the case where `cpu == smp_processor_id()`, which should never happen, and falls back to the local activation path if it does.

## Triggering Conditions

The race requires the following precise conditions:

- **SMP system with at least 3 CPUs**: Three CPUs are involved — one that runs the task, one that performs a migration, and one that initiates the wakeup. The waker CPU must be the task's previous CPU (before migration).
- **Weak memory ordering architecture**: The race depends on the CPU reordering a load of `p->cpu` before a load of `p->on_cpu`. On x86_64, LOCK instructions provide full memory barriers making this reordering extremely unlikely. On architectures like PowerPC or ARM, the weaker memory model makes the race more plausible.
- **TTWU_QUEUE sched feature enabled**: The `ttwu_queue_wakelist()` path must be taken, which requires the `TTWU_QUEUE` scheduler feature (enabled by default).
- **Task migration just before wake**: A task X must be migrated from CPU1 to CPU0 while CPU1 still has a stale view of `X->cpu == 1`. The migration updates `X->cpu = 0` under `rq(1)->lock`, but the waker on CPU1 reads `X->cpu` without proper ordering.
- **Task context-switches on new CPU, then sleeps**: After migration to CPU0, X must be scheduled (setting `X->on_cpu = 1`), then go to sleep (setting `X->state = TASK_UNINTERRUPTIBLE`), then call `schedule()` (which dequeues X and eventually clears `X->on_cpu = 0`).
- **Wakeup concurrent with schedule()**: CPU1 must call `try_to_wake_up(X)` while X is still in `schedule()` on CPU0 with `on_cpu == 1`. The waker observes `X->on_cpu == 1` (still running on CPU0) but `X->cpu == 1` (stale, from before migration). Since `cpu == smp_processor_id() == 1`, the wakelist path queues X locally on CPU1.
- **CFS scheduling class**: The NULL deref occurs in CFS-specific code (`check_preempt_wakeup` → `find_matching_se` → `is_same_group`), so the task must be a CFS task (the default scheduling class).
- **CONFIG_SMP enabled**: The entire wake_list optimization and the race are under `#ifdef CONFIG_SMP`.

The race is extremely rare — Paul McKenney's rcutorture took many CPU hours to trigger it. The probability increases with more CPUs, higher migration rates, and architectures with weaker memory ordering.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **KERNEL VERSION TOO OLD**: The bug was introduced in v5.8-rc1 by commit `c6e7bd7afaeb` ("sched/core: Optimize ttwu() spinning on p->on_cpu") and fixed in v5.8-rc3 by the commit under analysis. kSTEP supports Linux v5.15 and newer only. Since the fix was applied in v5.8-rc3 (released June 2020), the buggy code does not exist in any kernel version supported by kSTEP. By the time v5.15 was released (October 2021), this fix had been included for over a year.

2. **Memory ordering race**: Even if the kernel version were supported, this bug depends on CPU memory reordering — specifically, the processor must reorder two loads (`p->cpu` before `p->on_cpu`) that lack an ordering constraint. QEMU with TCG (Tiny Code Generator) emulates a sequentially consistent memory model, meaning loads are never reordered. The race would be impossible to trigger in QEMU's emulation environment regardless of the workload.

3. **Requires many CPUs and high contention**: The original reproduction required "many CPU hours of rcutorture" on real hardware, likely on large PowerPC systems with weak memory ordering. kSTEP's QEMU environment, while configurable with multiple vCPUs, cannot replicate the memory reordering behavior of real weakly-ordered hardware.

4. **Alternative reproduction methods**: The only viable reproduction approach would be to run rcutorture on real hardware with weak memory ordering (e.g., PowerPC, ARM64) running a kernel between v5.8-rc1 and v5.8-rc2 (before the fix). Even then, reproduction takes many CPU hours of stress testing and is not deterministic.

5. **What would need to change in kSTEP**: To support this class of bugs, kSTEP would need: (a) a QEMU backend that emulates weak memory ordering (e.g., using LKMM-aware memory model emulation), and (b) the ability to run pre-v5.15 kernels. Neither of these is a minor extension — they would require fundamental architectural changes to the emulation environment.
