# Core: mm_cid Lock Contention Performance Regression

**Commit:** `223baf9d17f25e2608dbdff7232c095c1e612268`
**Affected files:** `kernel/sched/core.c`, `kernel/sched/sched.h`
**Fixed in:** v6.4-rc1
**Buggy since:** v6.3-rc1 (introduced by `af7f588d8f73` "sched: Introduce per-memory-map concurrency ID")

## Bug Description

The commit `af7f588d8f73` introduced per-memory-map concurrency IDs (mm_cid), which are used by RSEQ (restartable sequences) to provide each thread with a compact, unique-per-mm integer identifier. This concurrency ID is allocated and freed on every context switch via `switch_mm_cid()`, which is called from `prepare_task_switch()`. The initial implementation used a per-mm `raw_spinlock_t cid_lock` to serialize all cid allocations and deallocations for a given `mm_struct`.

The problem is that on systems with many CPUs running multi-threaded workloads (many processes each with many threads), this per-mm `cid_lock` becomes extremely heavily contended. Every context switch on every CPU that involves a task with an `mm_struct` must acquire this lock to either free the outgoing task's cid (`mm_cid_put()`) or allocate a new cid for the incoming task (`mm_cid_get()`). On a 2-socket 112-core 224-CPU Intel Sapphire Rapids server running PostgreSQL with sysbench (56 threads), the overhead of `native_queued_spin_lock_slowpath` jumped from 0.03% to 43.18% of total CPU time, with `__schedule` consuming 49% of CPU time overall.

The root cause is a classic scalability bottleneck: a single lock protecting a shared bitmap, accessed from every CPU on every context switch. The lock is not the runqueue lock (as the reporter initially thought) but the `mm->cid_lock` introduced by the mm_cid feature. The contention is amplified by the fact that multi-threaded database workloads (like PostgreSQL) have frequent context switches between threads belonging to different memory spaces, each requiring both a `mm_cid_put()` (for the outgoing task) and a `mm_cid_get()` (for the incoming task), doubling the lock acquisition rate per context switch.

## Root Cause

The original `mm_cid_get()` and `mm_cid_put()` functions in `kernel/sched/sched.h` both acquired `mm->cid_lock` to manipulate the per-mm cidmask bitmap:

```c
static inline int mm_cid_get(struct mm_struct *mm)
{
    int ret;
    lockdep_assert_irqs_disabled();
    raw_spin_lock(&mm->cid_lock);
    ret = __mm_cid_get(mm);
    raw_spin_unlock(&mm->cid_lock);
    return ret;
}

static inline void mm_cid_put(struct mm_struct *mm, int cid)
{
    lockdep_assert_irqs_disabled();
    if (cid < 0)
        return;
    raw_spin_lock(&mm->cid_lock);
    __cpumask_clear_cpu(cid, mm_cidmask(mm));
    raw_spin_unlock(&mm->cid_lock);
}
```

These functions were called from `switch_mm_cid()`, which was invoked from `prepare_task_switch()` on every context switch. For a system with N CPUs running M processes each with T threads, any context switch involving threads from the same process contends on the same `mm->cid_lock`. With 56 threads in the PostgreSQL benchmark, every CPU switching between PostgreSQL worker threads hammered the same lock.

The `__mm_cid_get()` function performed `cpumask_first_zero()` on the cidmask to find an available cid, then set the bit. The `mm_cid_put()` function cleared the bit. Both operations are inherently fast, but the lock serialization around them created a severe bottleneck. The atomic operations for the lock itself (test-and-set on acquire, store on release) caused cache-line bouncing across all CPUs contending for the same `mm->cid_lock`.

Additionally, `switch_mm_cid()` had an optimization for same-mm context switches (thread-to-thread within the same process) that simply transferred the cid without lock operations. However, when switching between threads of *different* processes (the common case in database workloads serving many clients), both a `mm_cid_put()` on the previous mm and a `mm_cid_get()` on the next mm were required, each taking their respective lock.

The original implementation also placed `switch_mm_cid()` in `prepare_task_switch()` before the actual context switch, which meant the cid_lock was held while other scheduler operations were potentially in progress, further increasing hold time under contention.

## Consequence

The observable impact is a severe performance regression in multi-threaded workloads on high-core-count systems. The specific measurements from the bug report on a 2-socket/112-core/224-CPU Intel Sapphire Rapids server running PostgreSQL+sysbench show:

- **Before the bug (v6.2):** `__schedule` consumed 7.30% of CPU time; `native_queued_spin_lock_slowpath` was 0.03%.
- **After the bug (v6.3-rc with mm_cid):** `__schedule` consumed 49.01% of CPU time; `native_queued_spin_lock_slowpath` was 43.18% (a ~1400x increase).

This means nearly half of all CPU cycles were wasted spinning on lock contention rather than doing useful work. The throughput of the database workload was severely degraded. The regression scales with the number of CPUs and the number of threads — more CPUs and more threads produce worse contention. The reporter noted that using more than 56 threads made the contention even more severe on the 224-CPU machine.

This is purely a performance regression with no correctness implications — no crashes, no data corruption, no incorrect scheduling decisions. However, the magnitude of the regression (43% of CPU time in spinlock contention) makes it effectively a denial-of-service for high-throughput database workloads on modern server hardware.

## Fix Summary

The fix completely redesigns the mm_cid allocation mechanism to eliminate the per-mm `cid_lock` bottleneck. The key changes are:

**Per-mm/per-cpu cid tracking:** Instead of freeing cids immediately on context switch and re-allocating them under a lock, the fix introduces `struct mm_cid __percpu *pcpu_cid` in `mm_struct`. Each CPU keeps track of its currently allocated cid for each mm. When context-switching back to a thread from the same mm on the same CPU, the previously allocated cid is simply reused without any atomic operations. The per-cpu cid values are serialized by their respective runqueue locks, eliminating the need for the dedicated `cid_lock`.

**Lazy put with Dekker synchronization:** When switching away from an mm, instead of immediately freeing the cid, the fix sets a `MM_CID_LAZY_PUT` flag on the per-cpu cid. A lock-free Dekker memory ordering protocol is used to safely reclaim lazy-put cids: the scheduler's store to `rq->curr` and the remote-clear's cmpxchg on the per-cpu cid are ordered with memory barriers such that a remote observer can safely determine whether a cid is still actively in use. This avoids the need for any lock on the common-case context switch path.

**Migration handling:** The fix adds `sched_mm_cid_migrate_from()` (called from `set_task_cpu()`) and `sched_mm_cid_migrate_to()` (called from `activate_task()` for migrating tasks) to handle the transfer of cid values when tasks migrate between CPUs, keeping cid allocation compact. A periodic task-work callback (`task_mm_cid_work()`) clears stale per-cpu cid values that haven't been used for `SCHED_MM_CID_PERIOD_NS` (100ms), ensuring cids are eventually reclaimed even in the absence of migration.

**Moved switch_mm_cid() after memory barriers:** The `switch_mm_cid()` call is moved from `prepare_task_switch()` to after the mm switch in `context_switch()`, where it can leverage the existing memory barriers from `switch_mm_irqs_off()` and `mmgrab()` for the Dekker synchronization, rather than requiring additional barriers.

The result is that the common case (switching between threads on the same CPU) requires zero atomic operations for cid management, and even the cross-CPU case uses lock-free cmpxchg operations rather than a contended spinlock.

## Triggering Conditions

The following conditions are necessary to trigger this performance regression:

- **Kernel version:** v6.3-rc1 or later with `CONFIG_SCHED_MM_CID=y` (enabled by default when `CONFIG_SMP=y` and `CONFIG_RSEQ=y`).
- **CPU count:** A system with a large number of CPUs (56+ was used in the report, but contention would be observable with fewer CPUs). The contention scales with CPU count because more CPUs means more concurrent lock acquisitions.
- **Workload:** Multi-threaded processes where many threads from different processes (different `mm_struct`s) are rapidly context-switching on many CPUs. The PostgreSQL+sysbench workload is an example: multiple client connections (threads) share the same mm, and multiple PostgreSQL worker processes each with their own mm switch frequently.
- **Thread count:** The number of runnable threads should exceed the number of CPUs, or there should be frequent voluntary context switches (e.g., I/O waits) causing many switch events per second. The report used 56 threads on a system where this caused frequent cross-process context switches.
- **No specific topology requirements:** The bug manifests on any SMP system, though it is more severe on NUMA systems due to the additional cache-line transfer latency between sockets.

The bug is completely deterministic in the sense that it always manifests under the described workload — it is not a race condition or timing-dependent bug. It is a fundamental scalability limitation of the locking design.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

**1. Requires real userspace processes with mm_struct:** The mm_cid mechanism is only active for tasks that have an `mm_struct` (i.e., userspace processes). In `switch_mm_cid()`, the code checks `prev->mm_cid_active` and `next->mm_cid_active`, which are only set to 1 for tasks with valid mm_structs. kSTEP creates kernel-controlled tasks via `kstep_task_create()` and `kstep_kthread_create()`, which are kernel threads without mm_structs. The `mm_cid_active` field is 0 for kernel threads, so the entire mm_cid code path is skipped — `switch_mm_cid()` does nothing when both prev and next are kernel threads. Even `kstep_task_fork()` creates kernel-managed tasks that do not go through the full `fork()`/`exec()` lifecycle that establishes a real mm_struct with RSEQ support.

**2. Performance regression, not a correctness bug:** This bug manifests as lock contention (43% CPU time in spinlock slow path) rather than any incorrect scheduling behavior. kSTEP is designed to detect correctness issues by observing scheduler state (task placement, vruntime values, runqueue membership, etc.). There is no scheduler state that differs between the buggy and fixed kernels — both produce identical scheduling decisions. The only difference is how long the context switch takes due to lock contention. kSTEP has no mechanism to measure lock contention, spinlock wait times, or CPU utilization percentages. The `kstep_pass()`/`kstep_fail()` predicates cannot express "this context switch took too long."

**3. Requires many CPUs with real contention:** The lock contention is a function of the number of CPUs simultaneously trying to acquire the same `mm->cid_lock`. While QEMU can be configured with many virtual CPUs, TCG (QEMU's software CPU emulator) serializes execution across vCPUs, meaning there is no true parallel lock contention. Even with KVM acceleration, the virtual CPUs would need to be running a workload that generates frequent context switches. kSTEP's execution model (single driver function calling APIs sequentially) cannot generate concurrent lock acquisitions from multiple CPUs simultaneously.

**4. QEMU TCG has sequentially consistent memory:** Even if we could somehow trigger the mm_cid code path, QEMU TCG does not reorder memory operations. The Dekker memory ordering that the fix relies on (and that the old code implicitly needed) cannot be tested because QEMU never produces the weak memory orderings that make the new lock-free algorithm necessary. The performance difference between "lock-based" and "lock-free" approaches is invisible under sequential consistency.

**5. What would need to be added to kSTEP:** To reproduce this bug, kSTEP would need: (a) the ability to create real userspace processes with mm_structs (not just kernel threads), (b) the ability to measure lock contention or context switch latency, (c) true parallel execution on multiple CPUs, and (d) a workload generator that creates frequent cross-process context switches. This represents a fundamental architectural change to kSTEP, not a minor API addition.

**Alternative reproduction methods:** The bug is straightforwardly reproducible on real hardware using the reporter's instructions: run PostgreSQL in Docker with sysbench on a machine with 56+ CPUs. Profile with `perf top` or `perf record` and observe the `native_queued_spin_lock_slowpath` overhead. Alternatively, any multi-process/multi-threaded workload with frequent context switches on a high-core-count machine will exhibit the regression. Comparing perf profiles between kernels with and without commit `af7f588d8f73` (or with and without `CONFIG_SCHED_MM_CID=y`) confirms the issue.
