# Core: RSEQ mm_cid Fails to Compact After Thread/Affinity Reduction

**Commit:** `02d954c0fdf91845169cdacc7405b120f90afe01`
**Affected files:** `kernel/sched/sched.h`, `include/linux/mm_types.h`
**Fixed in:** v6.14-rc4
**Buggy since:** v6.13-rc1 (commit `7e019dcc470f` "sched: Improve cache locality of RSEQ concurrency IDs for intermittent workloads")

## Bug Description

The RSEQ (Restartable Sequences) subsystem assigns each thread of a process a concurrency ID (`mm_cid`) that is compact — ideally ranging from 0 to `min(nr_cpus_allowed, mm_users) - 1`. These concurrency IDs allow userspace applications (via the `rseq()` system call) to index per-thread data structures efficiently, taking advantage of cache locality.

Commit `7e019dcc470f` introduced a per-mm/CPU `recent_cid` field in `struct mm_cid` to improve cache locality for intermittent workloads. When a thread on a given CPU needs a new concurrency ID, the code first attempts to reuse the `recent_cid` stored for that CPU. This avoids scattering concurrency IDs across unrelated cache lines when threads run in bursts separated by more than 100ms.

However, this commit introduced a regression: the `max_nr_cid` counter (which tracks the highest concurrency ID ever allocated for the mm) is only ever incremented — it never decreases. When a process reduces its number of threads (threads exit) or restricts its CPU affinity mask, `max_nr_cid` remains at the old high-water mark. Furthermore, the `recent_cid` values cached per-CPU are never cleared unless a thread migrates, so even if threads exit and `mm_users` drops, the stale `recent_cid` values (which may be larger than the new desired upper bound) are still reused unconditionally.

The result is that mm_cid allocation never compacts downward after thread count reduction or affinity mask shrinkage. Applications that rely on mm_cid values being bounded by the current number of threads or allowed CPUs observe unexpectedly large concurrency IDs, defeating the purpose of the compaction guarantee.

## Root Cause

The root cause lies in the `__mm_cid_try_get()` function in `kernel/sched/sched.h`. In the buggy code, the function does the following:

```c
static inline int __mm_cid_try_get(struct task_struct *t, struct mm_struct *mm)
{
    struct cpumask *cidmask = mm_cidmask(mm);
    struct mm_cid __percpu *pcpu_cid = mm->pcpu_cid;
    int cid = __this_cpu_read(pcpu_cid->recent_cid);

    /* Try to re-use recent cid. This improves cache locality. */
    if (!mm_cid_is_unset(cid) && !cpumask_test_and_set_cpu(cid, cidmask))
        return cid;
    /* Expand cid allocation ... */
    cid = atomic_read(&mm->max_nr_cid);
    while (cid < READ_ONCE(mm->nr_cpus_allowed) && cid < atomic_read(&mm->mm_users)) {
        if (!atomic_try_cmpxchg(&mm->max_nr_cid, &cid, cid + 1))
            continue;
        if (!cpumask_test_and_set_cpu(cid, cidmask))
            return cid;
    }
    /* fallback: find first zero ... */
}
```

There are two specific problems:

**Problem 1: `max_nr_cid` never shrinks.** The `max_nr_cid` field is only ever incremented in the expansion loop (`atomic_try_cmpxchg(&mm->max_nr_cid, &cid, cid + 1)`). When threads exit or CPU affinity is reduced, `max_nr_cid` remains at the high-water mark. For example, if a process once had 64 threads, `max_nr_cid` stays at 64 even after all but 2 threads exit. When those 2 remaining threads call the expansion path, `cid = atomic_read(&mm->max_nr_cid)` reads 64, the while-loop condition `cid < atomic_read(&mm->mm_users)` (now 2) is false, so no expansion occurs. But the `max_nr_cid` remains inflated.

**Problem 2: `recent_cid` is reused without bound checking.** The `recent_cid` reuse path unconditionally accepts any previously cached concurrency ID: `if (!mm_cid_is_unset(cid) && !cpumask_test_and_set_cpu(cid, cidmask)) return cid;`. There is no check that `cid < max_nr_cid` or that `cid` is within the current allowed range (`min(nr_cpus_allowed, mm_users)`). This means a CPU that previously cached `recent_cid = 63` will continue to hand out CID 63 even if only 2 threads remain, completely bypassing any compaction.

Together, these two issues mean that after any reduction in thread count or CPU affinity, concurrency IDs remain scattered across a wide range and never converge toward smaller values.

## Consequence

The observable impact is that userspace applications using RSEQ concurrency IDs observe IDs that are larger than expected after reducing thread count or CPU affinity. This has several consequences:

**Data structure sizing issues:** Applications that size per-thread data structures based on the maximum expected mm_cid (bounded by `min(nr_cpus_allowed, nr_threads)`) will find that actual mm_cid values exceed this bound. An application might allocate an array of size `N` (for `N` current threads) and index it by mm_cid, but receive an mm_cid of 63 when only 2 threads are running, causing out-of-bounds access or requiring oversized allocations.

**Cache locality degradation:** The very purpose of mm_cid compaction is to keep concurrency IDs dense so that per-cid data stays cache-local. When IDs are scattered (e.g., IDs 0 and 63 for a 2-thread process), the associated per-cid data may span multiple cache lines or even different cache sets, negating the performance benefit of RSEQ concurrency IDs. The selftest included in the patchset (`mm_cid_compaction_test.c`) validates that mm_cid values compact after thread reduction and consistently fails on buggy kernels.

**Violated API contract:** The RSEQ API implicitly guarantees that mm_cid values are bounded by the number of threads and allowed CPUs. Applications and libraries (like `librseq`) that depend on this compaction property for correctness or performance will malfunction.

## Fix Summary

The fix introduces two changes to `__mm_cid_try_get()`:

**First**, it adds a `max_nr_cid` shrinkage mechanism at the start of the function. Before attempting any CID allocation, the code now computes `allowed_max_nr_cid = min(mm->nr_cpus_allowed, mm->mm_users)` and uses a compare-and-exchange loop to reduce `mm->max_nr_cid` if it exceeds this bound:

```c
max_nr_cid = atomic_read(&mm->max_nr_cid);
while ((allowed_max_nr_cid = min_t(int, READ_ONCE(mm->nr_cpus_allowed),
                                   atomic_read(&mm->mm_users))),
       max_nr_cid > allowed_max_nr_cid) {
    if (atomic_try_cmpxchg(&mm->max_nr_cid, &max_nr_cid, allowed_max_nr_cid)) {
        max_nr_cid = allowed_max_nr_cid;
        break;
    }
}
```

This ensures that whenever a thread calls `__mm_cid_try_get()`, the `max_nr_cid` is brought down to reflect the current reality (fewer threads or fewer allowed CPUs). The atomic CAS loop handles concurrent access safely.

**Second**, the `recent_cid` reuse path now checks that the cached CID is within the current `max_nr_cid` bound:

```c
cid = __this_cpu_read(pcpu_cid->recent_cid);
if (!mm_cid_is_unset(cid) && cid < max_nr_cid &&
    !cpumask_test_and_set_cpu(cid, cidmask))
    return cid;
```

The added `cid < max_nr_cid` check prevents reuse of stale, out-of-range concurrency IDs. If `recent_cid` is too large, the function falls through to the expansion or first-available paths, which will allocate a compact CID.

These changes correctly balance cache locality preservation (CIDs are still reused if within bounds) with compaction (IDs shrink back when threads exit or affinity is reduced). Applications that periodically expand and contract their thread pool will see `max_nr_cid` grow when threads are added and shrink when they are removed, and upon subsequent expansion, CID allocation will resume from the lower `max_nr_cid`, preserving cache locality.

## Triggering Conditions

To trigger this bug, the following conditions are required:

1. **CONFIG_SCHED_MM_CID must be enabled** (it is by default when CONFIG_RSEQ is enabled). The mm_cid mechanism is only compiled when this config option is set.

2. **A multi-threaded userspace process** must be running. The process must have created multiple threads (increasing `mm->mm_users`), causing `max_nr_cid` to be expanded to a value matching the peak thread count.

3. **Thread reduction**: Some of those threads must exit, reducing `mm->mm_users`. Alternatively, the process's CPU affinity mask can be restricted (reducing `mm->nr_cpus_allowed`), lowering the `min(nr_cpus_allowed, mm_users)` bound.

4. **Continued mm_cid allocation**: The remaining threads must continue to be scheduled (triggering `__mm_cid_try_get()` on context switch). On the buggy kernel, the allocated mm_cid values will remain scattered at the old high-water mark rather than compacting toward 0.

5. **No thread migration required**: The bug does not require any specific CPU topology or number of CPUs. Even a 2-CPU system can exhibit the issue. The bug is deterministic and 100% reproducible — any process that goes from many threads to few threads will exhibit non-compacting mm_cid values.

The selftest (`mm_cid_compaction_test.c`) demonstrates this by creating N threads pinned to different CPUs, waiting for them to all get unique mm_cid values, then terminating all but one thread, and checking that the remaining thread's mm_cid compacts to 0. On buggy kernels, the remaining thread retains a high mm_cid value indefinitely.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. Below is the detailed analysis:

### 1. Why kSTEP Cannot Reproduce This Bug

The mm_cid (memory-map concurrency ID) mechanism is fundamentally a **userspace process** feature. It is part of the RSEQ (Restartable Sequences) subsystem, which provides userspace threads with a compact, per-mm concurrency identifier accessible via the `rseq()` system call. The entire mm_cid allocation, compaction, and tracking infrastructure exists to serve **userspace threads that share an `mm_struct`** (i.e., threads created via `clone()` with `CLONE_VM`).

kSTEP creates kernel tasks via `kstep_task_create()` and kernel threads via `kstep_kthread_create()`. These are `PF_KTHREAD` tasks whose `task->mm` is NULL. The mm_cid mechanism is explicitly skipped for kernel threads — in `sched_mm_cid_before_execve()`, `switch_mm_cid()`, and related functions, the code checks `if (!mm)` or `if (t->flags & PF_KTHREAD)` and returns early. Kernel threads never participate in mm_cid allocation.

Even kSTEP's "user tasks" created with `kstep_task_create()` are not real userspace processes with a shared `mm_struct`. They are kernel-controlled entities that do not execute userspace code, do not call `rseq()`, and do not have the `mm_struct` lifecycle (fork, exec, thread creation/exit) that drives mm_cid allocation.

### 2. What Would Need to Be Added to kSTEP

To reproduce this bug, kSTEP would need fundamental architectural changes:

- **Real userspace process creation**: kSTEP would need the ability to create actual userspace processes with shared `mm_struct` instances. This would require something like `kstep_userspace_process_create()` that performs a `kernel_clone()` with `CLONE_VM` and sets up a real address space.

- **Thread lifecycle management**: The ability to create and destroy threads within a shared `mm_struct`, which updates `mm->mm_users` atomically. Thread exit must trigger the proper `mmput()` path.

- **CPU affinity control for userspace threads**: The ability to set and change CPU affinity for these userspace threads, triggering `mm->nr_cpus_allowed` updates.

- **RSEQ registration**: The threads would need to register with RSEQ via `sys_rseq()` and read back their mm_cid values.

- **Context switch through the mm_cid path**: The `switch_mm_cid()` function must be invoked during context switches, which only happens for tasks with a valid `mm`.

These are not minor additions — they represent a fundamental change to kSTEP's task model, moving from kernel-space task simulation to actual userspace process management. This goes far beyond adding a helper function or callback.

### 3. Alternative Reproduction Methods

The bug can be reproduced reliably outside kSTEP using the included selftest at `tools/testing/selftests/rseq/mm_cid_compaction_test.c`. The test:

1. Creates `N` threads (one per available CPU) pinned to distinct CPUs.
2. Each thread registers with RSEQ and reads its `mm_cid`.
3. Waits for all threads to get unique mm_cid values spanning `[0, N-1]`.
4. Terminates all threads except one.
5. Waits for the `task_mm_cid_work` compaction to run (100ms+ delay).
6. Checks that the surviving thread's mm_cid has compacted to 0.

On buggy kernels (with commit `7e019dcc470f` but without this fix), the test fails because the surviving thread retains a high mm_cid value. On fixed kernels, the test passes.

A simpler manual reproduction:

```c
// 1. Create 32 pthreads, each pinned to a different CPU
// 2. Have each thread read its rseq->mm_cid
// 3. Join 31 threads (leave 1 running)
// 4. Sleep 200ms (allow compaction work to run)
// 5. Read remaining thread's rseq->mm_cid
// Expected: mm_cid == 0 (fixed), mm_cid == large value (buggy)
```

This is purely a userspace test that can run on any Linux system with CONFIG_RSEQ enabled. No special hardware, topology, or kernel configuration is needed beyond a multi-CPU system.
