# Fair: wake_affine_idle() Miscounts nr_running Due to Delayed Dequeue

**Commit:** `aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.16-rc1
**Buggy since:** v6.12-rc1 (introduced by `152e11f6df29` "sched/fair: Implement delayed dequeue")

## Bug Description

The `wake_affine_idle()` function in the CFS scheduler is responsible for deciding whether a newly-woken task should be placed on the waking CPU (`this_cpu`) or the wakee's previous CPU (`prev_cpu`). When a synchronous wakeup (`WF_SYNC`) is in effect — meaning the waker expects to soon go to sleep — the function checks if the waker is the only task running on `this_cpu` by testing `cpu_rq(this_cpu)->nr_running == 1`. If so, it returns `this_cpu`, because the waker will imminently sleep and the CPU will become available for the wakee.

The delayed dequeue feature (EEVDF, commit `152e11f6df29`) keeps sleeping tasks enqueued on the runqueue until their accumulated lag has elapsed. These "sched-delayed" tasks are sleeping but still counted in `rq->nr_running`. This means that `nr_running` can be greater than 1 even when the waker is the only truly runnable task on the CPU, because one or more delayed-dequeued (sleeping) tasks artificially inflate the count.

As a consequence, the `sync` path in `wake_affine_idle()` fails to identify the waker's CPU as a suitable target. The function falls through and may select the previous CPU or return `nr_cpumask_bits` (meaning no affine decision), leading to unnecessary task migrations and suboptimal placement. This defeats the purpose of `WF_SYNC` wake-affine optimization, which is designed to co-locate waker/wakee pairs that exhibit a producer-consumer pattern.

The regression was confirmed by Tianchen Ding from Alibaba using `lmbench lat_ctx` benchmarks on both x86 (192 CPUs, Intel SPR) and aarch64 (128 CPUs) servers, showing context-switch latency regressing by 2-3x when delayed dequeue was enabled, with CPU migrations increasing more than 100-fold.

## Root Cause

The root cause is in the `wake_affine_idle()` function in `kernel/sched/fair.c`. Before the fix, the sync check was:

```c
if (sync && cpu_rq(this_cpu)->nr_running == 1)
    return this_cpu;
```

The value `rq->nr_running` counts all tasks that are "enqueued" on the runqueue, which after commit `152e11f6df29` includes sched-delayed tasks. Sched-delayed tasks are tasks that have gone to sleep (called `dequeue_task_fair()` with `DEQUEUE_SLEEP`) but whose dequeue was deferred because they still have positive lag — the scheduler keeps them on the RB-tree competing for virtual runtime until they become eligible (zero-lag point) or get picked again, at which point they are actually dequeued. These tasks are tracked in `cfs_rq->h_nr_queued` but not in `cfs_rq->h_nr_runnable`, so the difference `h_nr_queued - h_nr_runnable` represents the count of delayed tasks.

The problem is that `rq->nr_running` includes these delayed tasks. If the waker is the sole actually-running task on `this_cpu`, but there are N delayed-dequeued tasks also enqueued, then `rq->nr_running` equals `1 + N`, and the check `nr_running == 1` fails. The function then falls through to the `available_idle_cpu(prev_cpu)` check or returns `nr_cpumask_bits`, both of which bypass the sync-wake affine optimization.

This matters because `wake_affine_idle()` is called from `wake_affine()` when the `WA_IDLE` scheduling feature is enabled (which is the default). It is the first heuristic tried during wake-up CPU selection in `select_task_rq_fair()`. When it fails to produce a decision, the scheduler falls through to `wake_affine_weight()` or the slow path (`sched_balance_find_dst_group`), which may select a different CPU, causing the wakee to be placed far from the waker — exactly the opposite of what `WF_SYNC` intends.

The issue is particularly acute in producer-consumer workloads where a waker repeatedly wakes a wakee and then blocks. The waker and wakee benefit from staying on the same CPU for cache locality. With delayed dequeue inflating `nr_running`, the wake-affine logic fails to co-locate them, leading to excessive migrations between CPUs.

## Consequence

The observable impact is a significant performance regression in workloads that rely on synchronous wake-up affinity. Benchmarks from Tianchen Ding showed:

- **Intel SPR (192 CPUs):** `lmbench lat_ctx` latency went from 4.02 μs (no delayed dequeue) to 9.71 μs (with delayed dequeue) — a **2.4x regression**. With the fix applied, latency dropped to 3.86 μs, actually slightly better than the baseline without delayed dequeue.
- **Aarch64 (128 CPUs):** Latency went from 5.62 μs to 14.82 μs — a **2.6x regression**. With the fix: 4.66 μs.

The symptoms include:
- CPU migrations increasing by more than **100x** compared to the non-delayed-dequeue baseline.
- Higher `nr_wakeups_migrate`, `nr_wakeups_remote`, and `nr_wakeups_affine_attempts` scheduler statistics.
- Lower `nr_wakeups_local`, confirming that tasks are being placed on remote CPUs instead of staying local.

This regression affects any real-world workload exhibiting a producer-consumer or client-server pattern where threads use synchronous wakeups (e.g., pipe-based communication, futex wakeups with `FUTEX_WAKE`, `epoll` with `EPOLL_CTL_ADD` combined with `wake_up_sync()`). The degradation is proportional to the number of delayed-dequeued tasks on the waker's CPU, which can accumulate when multiple CFS tasks sleep in quick succession.

## Fix Summary

The fix introduces a new helper function `cfs_h_nr_delayed()` and modifies the sync check in `wake_affine_idle()` to subtract the count of delayed tasks from `nr_running`:

```c
static inline unsigned int cfs_h_nr_delayed(struct rq *rq)
{
    return (rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable);
}
```

The `cfs_rq->h_nr_queued` field counts all tasks enqueued on the CFS hierarchy (including delayed tasks), while `cfs_rq->h_nr_runnable` counts only the truly runnable tasks (excluding delayed ones). Their difference gives the exact count of sched-delayed tasks.

The sync check in `wake_affine_idle()` is changed from:

```c
if (sync && cpu_rq(this_cpu)->nr_running == 1)
    return this_cpu;
```

to:

```c
if (sync) {
    struct rq *rq = cpu_rq(this_cpu);
    if ((rq->nr_running - cfs_h_nr_delayed(rq)) == 1)
        return this_cpu;
}
```

This correctly computes the count of genuinely running tasks by subtracting the delayed ones. The fix ensures that sched-delayed tasks do not prevent the sync-wake affine optimization from firing. The fix is correct because delayed tasks are sleeping and will not compete with the wakee for CPU time — they are kept on the runqueue only for EEVDF lag tracking purposes and will be fully dequeued when next picked or when their lag expires.

## Triggering Conditions

The bug requires the following conditions to trigger:

1. **Delayed dequeue must be enabled**: The `DELAY_DEQUEUE` sched feature must be active (it is enabled by default since v6.12-rc1). This is what causes sleeping tasks to remain on the runqueue.

2. **WF_SYNC wakeup**: The waker must perform a synchronous wakeup. This happens when kernel code calls `wake_up_sync()`, `wake_up_interruptible_sync()`, or any wakeup path that sets the `WF_SYNC` flag. In practice, this occurs in pipe communication (`pipe_write()` calls `wake_up_interruptible_sync_poll()`), certain futex operations, and other kernel synchronization primitives.

3. **WA_IDLE sched feature enabled**: The `WA_IDLE` scheduling feature must be active (it is enabled by default). This is what causes `wake_affine_idle()` to be invoked.

4. **Delayed tasks present on the waker's CPU**: There must be at least one sched-delayed task on `this_cpu` (the waker's CPU) at the time of the wakeup decision. This happens when CFS tasks on that CPU recently went to sleep but haven't yet had their lag expire. The more tasks that have recently slept on the waker's CPU, the more inflated `nr_running` becomes.

5. **Waker's CPU is not idle**: The `this_cpu` must not be flagged as idle (otherwise the first check `available_idle_cpu(this_cpu)` would handle it before reaching the sync path). Since the waker is currently running, the CPU is not idle.

6. **SMP configuration**: The bug only manifests on SMP systems with at least 2 CPUs, since task placement decisions are only meaningful when there are multiple CPUs.

7. **Waker and wakee on different CPUs previously**: For `wake_affine()` to be called, the wakee's `prev_cpu` should differ from the waker's current CPU (otherwise the `cpu != prev_cpu` check in `select_task_rq_fair()` prevents `wake_affine()` from being called), or if they're on the same CPU the function simply returns `prev_cpu`.

The bug is reliably triggerable whenever these conditions are met. It is not a race condition — it is a deterministic logic error. The key scenario is: the waker is on CPU A with some delayed-dequeued tasks, the wakee previously ran on CPU B, and the waker does a sync wakeup. The buggy code sees `nr_running > 1` (due to delayed tasks) and fails to return `this_cpu`, causing the wakee to not be placed on CPU A.

## Reproduce Strategy (kSTEP)

The bug can be reproduced using kSTEP by constructing a scenario where:
- A waker kthread performs a synchronous wakeup of a wakee kthread.
- The waker's CPU has delayed-dequeued tasks inflating `nr_running`.
- We observe whether the wakee is placed on the waker's CPU (correct behavior) or elsewhere (buggy behavior).

### Step-by-step plan:

1. **Configure QEMU with at least 3 CPUs** (CPU 0 reserved for driver, CPUs 1 and 2 for test tasks). No special topology needed — default flat SMP is sufficient.

2. **Ensure `DELAY_DEQUEUE` is enabled**: This is the default in v6.12+. No sysctl change needed.

3. **Create "filler" CFS tasks on CPU 1** (the waker's CPU): Create 2-3 CFS kthreads and pin them to CPU 1 using `kstep_kthread_bind()`. Start them spinning with `kstep_kthread_start()`. These tasks will later be blocked to create delayed-dequeued entries on the runqueue.

4. **Create the waker kthread on CPU 1**: Create a kthread, bind it to CPU 1, and start it spinning.

5. **Create the wakee kthread on CPU 2**: Create a kthread, bind it to CPU 2 initially, start it, and then block it using `kstep_kthread_block()`. This ensures the wakee's `prev_cpu` is CPU 2.

6. **Let all tasks stabilize**: Call `kstep_tick_repeat(50)` to let the scheduler settle and all tasks accumulate runtime and lag on their respective CPUs.

7. **Block the filler tasks**: Call `kstep_kthread_block()` on each filler task. Because delayed dequeue is enabled, these tasks will remain in `rq->nr_running` on CPU 1 as delayed-dequeued entities even though they are sleeping. Allow a tick or two (`kstep_tick_repeat(2)`) to process the blocks but not enough for the delayed entries to expire.

8. **Trigger the sync wakeup**: Call `kstep_kthread_syncwake(waker, wakee)`. The waker on CPU 1 will invoke `__wake_up_sync()` on the wakee's wait queue, which sets `WF_SYNC` and goes through `select_task_rq_fair()` → `wake_affine()` → `wake_affine_idle()`.

9. **Allow one tick for the wakeup to complete**: `kstep_tick_repeat(3)`.

10. **Check the wakee's CPU**: Use `task_cpu(wakee)` to determine where the wakee was placed.
    - **Buggy kernel**: The wakee will likely NOT be on CPU 1, because `nr_running` on CPU 1 includes the delayed filler tasks and exceeds 1, so the sync check fails. The wakee stays on its `prev_cpu` (CPU 2) or gets placed elsewhere.
    - **Fixed kernel**: The wakee SHOULD be on CPU 1, because the fix correctly subtracts the delayed count, sees that only 1 truly running task exists (the waker), and returns `this_cpu`.

11. **Report pass/fail**: Use `kstep_pass()` or `kstep_fail()` based on whether `task_cpu(wakee)` equals the waker's CPU (CPU 1).

### Additional observations for debugging:

- Use `KSYM_IMPORT` to access `cfs_rq` internals and log `rq->nr_running`, `cfs_rq->h_nr_queued`, `cfs_rq->h_nr_runnable` on CPU 1 at the time of the wakeup (in an `on_tick_begin` callback).
- Log `task_cpu(wakee)` before and after the sync wakeup.
- On the buggy kernel, expect `rq->nr_running >= 2` (1 waker + N delayed fillers) and the wakee ending up on CPU 2.
- On the fixed kernel, expect the effective running count to be 1, and the wakee ending up on CPU 1.

### Potential complications:

- The delayed tasks may be quickly picked and dequeued if there is no competition (as Peter Zijlstra noted in the mailing list discussion). To ensure they remain delayed at the time of the sync wakeup, minimize the number of ticks between blocking the fillers and triggering the wakeup. The fillers need enough prior runtime to accumulate lag so that their delayed dequeue persists for at least 1-2 ticks.
- The `wake_wide()` check in `select_task_rq_fair()` may reject wake-affine entirely for tasks with high `wakee_flips`. Using fresh kthreads with no wakeup history avoids this issue.
- The `find_energy_efficient_cpu()` path could preempt `wake_affine()` if EAS is enabled. Ensure EAS is not active (no asymmetric CPU capacities or energy model) or disable it via sysctl.
- The wakee must have its affinity set to allow both CPU 1 and CPU 2 when the sync wakeup happens, otherwise the scheduler cannot place it on CPU 1. After initially binding the wakee to CPU 2 and starting it, re-bind it to CPUs 1-2 before the sync wakeup using `kstep_kthread_bind()`.

### Expected kernel version guard:

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
```

This guards the driver to only compile on kernels v6.12+ where delayed dequeue exists.
