# Core: Sleeping-in-Atomic in task_tick_mm_cid() Due to KASAN Page Allocation

**Commit:** `73ab05aa46b02d96509cb029a8d04fca7bbde8c7`
**Affected files:** kernel/sched/core.c, kernel/task_work.c, include/linux/task_work.h
**Fixed in:** v6.12-rc4
**Buggy since:** v6.4-rc1 (introduced by commit `223baf9d17f2` "sched: Fix performance regression introduced by mm_cid")

## Bug Description

When both CONFIG_KASAN (Kernel Address Sanitizer) and CONFIG_PREEMPT_RT (fully preemptible real-time kernel) are enabled, the function `task_tick_mm_cid()` in `kernel/sched/core.c` triggers a "BUG: sleeping function called from invalid context" splat. The root cause is that `task_tick_mm_cid()` is called from within `sched_tick()` while holding the per-CPU runqueue lock (`rq->__lock`), which is a `raw_spinlock_t`. Under PREEMPT_RT, regular `spin_lock_t` locks are converted to sleeping locks (`rt_mutex`-based), but `raw_spinlock_t` remains a true non-sleeping spinlock. Code executing under a `raw_spinlock_t` must never invoke any function that might sleep.

The `task_tick_mm_cid()` function calls `task_work_add()`, which in turn calls `kasan_record_aux_stack()` to record the call stack for KASAN reporting. This KASAN function calls `stack_depot_save_flags()`, which may need to allocate new pages via `alloc_pages()` if the stack depot buffer is full. Under PREEMPT_RT, the page allocator's internal locking (specifically `rmqueue_bulk()` calling `rt_spin_lock()`) is a sleeping operation. Sleeping while holding the raw `rq->__lock` violates the PREEMPT_RT locking rules and produces the splat.

This bug was introduced in v6.4-rc1 by commit `223baf9d17f2`, which added the `task_work_add()` call to `task_tick_mm_cid()` as part of a fix for a performance regression in the mm_cid (memory map concurrency ID) subsystem. The mm_cid subsystem tracks per-CPU concurrency IDs for memory maps to optimize RSEQ (restartable sequences) operations. The periodic tick scan (`task_tick_mm_cid()`) schedules a deferred work item (`task_mm_cid_work`) to compact CID allocations, but the `task_work_add()` call was not safe to invoke from atomic/raw spinlock context when KASAN instrumentation triggers page allocation.

## Root Cause

The call chain that triggers the bug is:

```
sched_tick()                          [holds rq->__lock (raw_spinlock_t)]
  → task_tick_mm_cid(rq, curr)
    → task_work_add(curr, work, TWA_RESUME)
      → kasan_record_aux_stack(work)
        → kasan_save_stack()
          → stack_depot_save_flags()
            → alloc_pages_mpol_noprof()
              → __alloc_pages_noprof()
                → get_page_from_freelist()
                  → rmqueue()
                    → rmqueue_pcplist()
                      → __rmqueue_pcplist()
                        → rmqueue_bulk()
                          → rt_spin_lock()  ← SLEEPS on PREEMPT_RT!
```

Under `CONFIG_PREEMPT_RT`, `spin_lock_t` is implemented as an `rt_mutex`-based sleeping lock. The page allocator's zone lock, acquired by `rmqueue_bulk()`, is a `spin_lock_t`. When this becomes a sleeping lock on PREEMPT_RT, attempting to acquire it while holding a `raw_spinlock_t` (the rq lock) constitutes an illegal sleeping-in-atomic-context violation.

The `kasan_record_aux_stack()` function is normally safe to call in most contexts because KASAN's stack depot uses pre-allocated memory. However, when the stack depot's internal buffer runs out of space, `stack_depot_save_flags()` falls back to `alloc_pages()` to expand its buffer. This expansion path is not safe under raw spinlocks on PREEMPT_RT kernels.

The `task_tick_mm_cid()` function in `kernel/sched/core.c` (around line 10451) is called on every scheduler tick for the currently running task. It checks whether the current task has an mm_struct, is not exiting or a kthread, and whether it is time for a periodic CID compaction scan. If all conditions are met, it calls `task_work_add()` to schedule the deferred work:

```c
void task_tick_mm_cid(struct rq *rq, struct task_struct *curr)
{
    struct callback_head *work = &curr->cid_work;
    unsigned long now = jiffies;

    if (!curr->mm || (curr->flags & (PF_EXITING | PF_KTHREAD)) ||
        work->next != work)
        return;
    if (time_before(now, READ_ONCE(curr->mm->mm_cid_next_scan)))
        return;
    task_work_add(curr, work, TWA_RESUME);  /* BUG: may sleep via KASAN */
}
```

The `task_work_add()` function in `kernel/task_work.c` unconditionally calls `kasan_record_aux_stack(work)` for all non-NMI notify modes. There was no mechanism to disable the page allocation path in this KASAN call, so every invocation from a raw-spinlock context was potentially unsafe on PREEMPT_RT.

## Consequence

The observable consequence is a kernel BUG splat indicating "sleeping function called from invalid context":

```
[   63.696416] BUG: sleeping function called from invalid context at kernel/locking/spinlock_rt.c:48
[   63.696416] in_atomic(): 1, irqs_disabled(): 1, non_block: 0, pid: 610, name: modprobe
[   63.696416] preempt_count: 10001, expected: 0
[   63.696416] RCU nest depth: 1, expected: 1
```

This splat is produced by the `might_sleep()` check inside `rt_spin_lock()`. On PREEMPT_RT kernels, this is a serious correctness violation. While the kernel may continue running after the splat (it is a WARN-level diagnostic, not a panic), it indicates that a lock ordering violation has occurred. In the worst case, this could lead to:

1. **Lock inversion deadlocks**: If the page allocator's zone lock and the rq lock are ever held in the reverse order on another CPU, a deadlock could occur.
2. **Priority inversion**: Under PREEMPT_RT, sleeping locks implement priority inheritance. A high-priority real-time task holding the rq lock could be forced to sleep waiting for a lower-priority task holding the zone lock, defeating the purpose of PREEMPT_RT.
3. **Latency violations**: On real-time systems, unexpected sleeping in the scheduler tick path adds unbounded latency to the scheduling critical section.

The bug is deterministic once the stack depot buffer is full and needs expansion — it will trigger every time `task_tick_mm_cid()` calls `task_work_add()` on a KASAN+PREEMPT_RT kernel with a userspace task whose mm_cid scan timer has expired.

## Fix Summary

The fix introduces a new `TWAF_NO_ALLOC` flag (value `0x0100`) in the `enum task_work_notify_mode` in `include/linux/task_work.h`. This flag is designed to be OR'd with the existing notification mode values. A corresponding `TWA_FLAGS` mask (`0xff00`) is added to separate flags from the base notification mode.

In `task_work_add()` (`kernel/task_work.c`), the function is modified to extract and strip the flags from the `notify` parameter before processing the notification mode. When `TWAF_NO_ALLOC` is set, the function calls `kasan_record_aux_stack_noalloc(work)` instead of `kasan_record_aux_stack(work)`. The `_noalloc` variant calls `stack_depot_save_flags()` with a flag that prevents new page allocation — if the stack depot buffer is full, it simply fails to record the stack trace rather than attempting to allocate memory.

In `kernel/sched/core.c`, the `task_tick_mm_cid()` function is changed from:
```c
task_work_add(curr, work, TWA_RESUME);
```
to:
```c
task_work_add(curr, work, TWA_RESUME | TWAF_NO_ALLOC);
```

This ensures that the page allocation path is never taken when `task_work_add()` is called from the scheduler tick path under the rq lock. The trade-off is that KASAN stack traces may occasionally be incomplete if a new page was needed to store the stack trace, but this is considered an acceptable compromise since the mm_cid work path is not a common source of KASAN-reportable bugs.

## Triggering Conditions

The bug requires all of the following conditions to be simultaneously true:

1. **CONFIG_KASAN enabled**: The kernel must be compiled with KASAN (Kernel Address Sanitizer) support. Without KASAN, `kasan_record_aux_stack()` is a no-op and no page allocation occurs.
2. **CONFIG_PREEMPT_RT enabled**: The kernel must be a fully preemptible real-time kernel. Without PREEMPT_RT, `spin_lock_t` is a regular non-sleeping spinlock, and `rmqueue_bulk()` does not sleep — so there is no sleeping-in-atomic violation (though the allocation itself is still technically questionable under a raw spinlock, it would not trigger the BUG splat).
3. **A userspace process running**: The current task must have a non-NULL `mm` field and must not have `PF_KTHREAD` or `PF_EXITING` set. Kernel threads and exiting tasks are filtered out by the early-return check in `task_tick_mm_cid()`.
4. **mm_cid scan timer expired**: The jiffies counter must have advanced past `curr->mm->mm_cid_next_scan`. This timer is initialized to `jiffies + msecs_to_jiffies(MM_CID_SCAN_DELAY)` when the mm is first used, where `MM_CID_SCAN_DELAY` is typically 100ms.
5. **work->next == work**: The task's `cid_work` callback head must not already be queued (the sentinel check `work->next != work` ensures no double-add).
6. **Stack depot buffer full**: The KASAN stack depot must have exhausted its pre-allocated buffer, causing `stack_depot_save_flags()` to attempt a page allocation. This is more likely to occur on systems that have been running for some time and have accumulated many unique stack traces.

The race or timing window is relatively wide: once the mm_cid scan timer expires (every ~100ms by default), every scheduler tick for an eligible userspace task will trigger the `task_work_add()` call. The actual BUG splat depends on whether the stack depot needs to allocate — this is non-deterministic but becomes increasingly likely as the system runs longer and more unique stacks are recorded.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Requires CONFIG_KASAN + CONFIG_PREEMPT_RT Kernel Configuration

The bug fundamentally requires two specific kernel configuration options to be simultaneously enabled:

- **CONFIG_KASAN**: Without KASAN, the `kasan_record_aux_stack()` function compiles to a no-op (a static inline empty function). No page allocation is attempted, and the bug is completely invisible. The entire call chain that leads to the sleeping violation does not exist without KASAN.

- **CONFIG_PREEMPT_RT**: Without PREEMPT_RT, `spin_lock_t` is a standard non-sleeping spinlock. Even if KASAN triggers a page allocation under the rq lock, the `rmqueue_bulk()` → `spin_lock()` path does not sleep, so there is no sleeping-in-atomic violation. The BUG splat only fires because `rt_spin_lock()` calls `might_sleep()`, which detects the invalid context.

kSTEP builds its test kernels with a standard configuration. Enabling both KASAN and PREEMPT_RT simultaneously is an unusual and heavyweight configuration that significantly changes kernel behavior (KASAN adds substantial memory overhead and PREEMPT_RT converts all spinlocks to sleeping locks). kSTEP's build system and QEMU test infrastructure are not designed for this specific configuration combination.

### 2. Bug Manifestation Is a Locking Violation, Not a Scheduling Behavior Change

The bug does not produce any incorrect scheduling decision, task placement error, priority inversion, starvation, or any other scheduling-observable behavior. The bug manifests as a `BUG: sleeping function called from invalid context` kernel warning. kSTEP's observation mechanisms (`kstep_pass()`, `kstep_fail()`, `kstep_output_curr_task()`, `kstep_output_nr_running()`, etc.) monitor scheduling state — they cannot detect locking violations or sleeping-in-atomic-context warnings.

Even if we could trigger the `task_tick_mm_cid()` → `task_work_add()` path, there would be no way to distinguish the buggy behavior from the correct behavior through kSTEP's observation API. The scheduling decisions would be identical regardless of whether the KASAN stack recording succeeds, fails, or causes a locking violation.

### 3. What Would Need to Change

To reproduce this bug, kSTEP would need:

- **Kernel build with CONFIG_KASAN + CONFIG_PREEMPT_RT**: The kSTEP build system (`Makefile` and kernel config) would need to support building kernels with these options enabled. This is a significant infrastructure change since KASAN+RT kernels have different performance characteristics and memory requirements.

- **Kernel log monitoring**: kSTEP would need a mechanism to detect kernel warnings/BUG splats in `dmesg` output and report them as test failures. Currently kSTEP observes scheduling state, not kernel diagnostic output. A new API like `kstep_check_dmesg_for_bugs()` or a callback `on_kernel_warning()` would be needed.

- **Stack depot exhaustion**: The test would need to fill the KASAN stack depot's pre-allocated buffer to force the page allocation path. This would require generating many unique stack traces, which is difficult from a kernel module.

### 4. Alternative Reproduction Methods

The bug can be reproduced outside kSTEP by:

1. Building a kernel with both `CONFIG_KASAN=y` and `CONFIG_PREEMPT_RT=y` (and `CONFIG_PREEMPT_RT_FULL=y` if applicable).
2. Booting the kernel in QEMU or on real hardware.
3. Running any userspace workload (even a simple `sleep` or `modprobe` as shown in the original report) and waiting for the mm_cid scan timer to expire (~100ms).
4. Monitoring `dmesg` for the "sleeping function called from invalid context" BUG splat.

The original reporter observed the bug with PID 610 (`modprobe`) at approximately 63 seconds after boot, indicating it can trigger relatively quickly once the stack depot fills up.
