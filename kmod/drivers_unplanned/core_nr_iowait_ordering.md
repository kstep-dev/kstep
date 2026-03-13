# Core: nr_iowait decrement-before-increment race in ttwu() vs schedule()

**Commit:** `ec618b84f6e15281cc3660664d34cd0dd2f2579e`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.10-rc5
**Buggy since:** v5.8-rc1 (introduced by commit `c6e7bd7afaeb` "sched/core: Optimize ttwu() spinning on p->on_cpu")

## Bug Description

The `rq->nr_iowait` atomic counter tracks how many tasks on a given runqueue are sleeping in I/O wait state. This counter is used by the kernel to compute IO-wait CPU time percentages and feeds into load average calculations. The counter is incremented in `__schedule()` when a task with `in_iowait` set goes to sleep, and decremented in `try_to_wake_up()` when such a task is woken up.

Commit `c6e7bd7afaeb` ("sched/core: Optimize ttwu() spinning on p->on_cpu") introduced an optimization that allows `try_to_wake_up()` to bypass the `smp_cond_load_acquire(&p->on_cpu, !VAL)` spin by queueing the wakeup via `ttwu_queue_wakelist()` on the CPU that still owns `p->on_cpu`. This means the wakeup path in `try_to_wake_up()` can now proceed and execute the `atomic_dec(&task_rq(p)->nr_iowait)` before `__schedule()` on the sleeping CPU has completed and executed the corresponding `atomic_inc(&rq->nr_iowait)`.

This ordering violation causes `nr_iowait` to temporarily go negative (wrapping to a very large unsigned value when read), producing wildly incorrect IO-wait statistics. The race is especially pronounced under heavy concurrent wake-up workloads on SMP systems, particularly on architectures like ARM64 where memory ordering subtleties can exacerbate the issue.

The broader LKML discussion that triggered this fix was initially about "exploding" load averages on ARM64 build servers running hackbench workloads. The investigation revealed that the `ttwu_queue_wakelist()` optimization broke assumptions about the ordering of task state transitions between `schedule()` and `try_to_wake_up()`.

## Root Cause

In the buggy code, `try_to_wake_up()` performed the `nr_iowait` decrement early, immediately after checking `p->on_rq` and before any synchronization with the sleeping CPU:

```c
// try_to_wake_up() — BUGGY version
smp_rmb();
if (READ_ONCE(p->on_rq) && ttwu_runnable(p, wake_flags))
    goto unlock;

if (p->in_iowait) {
    delayacct_blkio_end(p);
    atomic_dec(&task_rq(p)->nr_iowait);  // DECREMENT happens here, TOO EARLY
}

// ... later, may go through ttwu_queue_wakelist() which skips on_cpu spin
```

Meanwhile, on the CPU where the task is being descheduled, `__schedule()` does:

```c
// __schedule()
deactivate_task(rq, prev, ...);  // sets on_rq = 0

if (prev->in_iowait)
    atomic_inc(&rq->nr_iowait);  // INCREMENT happens here, AFTER deactivate
```

The critical race window is:

1. **CPU A** (`schedule()`): Calls `deactivate_task()`, setting `p->on_rq = 0`.
2. **CPU B** (`ttwu()`): Reads `p->on_rq == 0`, skips `ttwu_runnable()`.
3. **CPU B** (`ttwu()`): Sees `p->in_iowait`, calls `atomic_dec(&task_rq(p)->nr_iowait)`. The counter goes from 0 to -1 (unsigned underflow).
4. **CPU A** (`schedule()`): Reaches `if (prev->in_iowait)` and calls `atomic_inc(&rq->nr_iowait)`. Counter goes from -1 back to 0.

Before commit `c6e7bd7afaeb`, this race was not possible because `try_to_wake_up()` would spin on `smp_cond_load_acquire(&p->on_cpu, !VAL)` which ensured `schedule()` had fully completed (including the `nr_iowait` increment) before the wakeup path could decrement it. After `c6e7bd7afaeb`, the new `ttwu_queue_wakelist()` path can bypass this spin entirely when `p->on_cpu` is still set, queueing the wakeup as an IPI to the target CPU. This means the decrement can race ahead of the increment.

Additionally, even when the task is not going through `ttwu_queue_wakelist()`, the decrement was placed before `smp_cond_load_acquire(&p->on_cpu, !VAL)`, so even the non-wakelist path was racy.

## Consequence

The immediate consequence is that `rq->nr_iowait` can temporarily hold a negative value (which appears as a very large positive value when cast to unsigned). This has several observable effects:

**Incorrect IO-wait statistics:** The `/proc/stat` CPU time accounting uses `nr_iowait` to determine how much time to attribute to IO-wait vs idle. A negative/underflowed value causes the kernel to report massively inflated IO-wait percentages. Tools like `top`, `mpstat`, `sar`, and monitoring systems relying on `/proc/stat` will show wildly inaccurate IO-wait numbers.

**Load average inflation:** As observed by Mel Gorman on ARM64 build servers, the load average can "explode" to values far exceeding the number of CPUs. In the reported case, a 96-CPU ARM64 machine showed load averages above 200 even after all workloads had stopped, when it should have dropped to near 0. The load average remained stuck for extended periods because the accounting error accumulates and takes a long time to self-correct (if it ever does). This was reproduced by running `hackbench-process-pipes` while overcommitting the machine and then observing that load averages did not drop back to normal within 10 minutes.

**PSI and cpufreq impact:** The `nr_iowait` value also affects pressure stall information (PSI) and CPU frequency governor decisions via schedutil. Incorrect iowait accounting can lead to suboptimal frequency scaling decisions, as the governor may interpret high iowait as an indication to keep frequencies elevated.

## Fix Summary

The fix moves the `nr_iowait` decrement (and `delayacct_blkio_end()`) from its early position in `try_to_wake_up()` to two later, properly-ordered locations:

**Case 1: Task migration (WF_MIGRATED).** When `select_task_rq()` chooses a different CPU than the task's current CPU, the decrement is placed after `smp_cond_load_acquire(&p->on_cpu, !VAL)`. This ensures that `schedule()` on the original CPU has fully completed (including the `nr_iowait` increment) before the decrement occurs. The code now reads:

```c
smp_cond_load_acquire(&p->on_cpu, !VAL);
cpu = select_task_rq(p, p->wake_cpu, SD_BALANCE_WAKE, wake_flags);
if (task_cpu(p) != cpu) {
    if (p->in_iowait) {
        delayacct_blkio_end(p);
        atomic_dec(&task_rq(p)->nr_iowait);
    }
    wake_flags |= WF_MIGRATED;
    ...
}
```

**Case 2: Same-CPU wakeup or wakelist path (non-WF_MIGRATED).** When the task stays on the same CPU (or is queued via `ttwu_queue_wakelist()`), the decrement is deferred to `ttwu_do_activate()`, which runs under the `rq->lock` of the destination runqueue. This provides proper serialization against `schedule()`:

```c
static void ttwu_do_activate(struct rq *rq, struct task_struct *p,
                              int wake_flags, struct rq_flags *rf)
{
    ...
#ifdef CONFIG_SMP
    if (wake_flags & WF_MIGRATED)
        en_flags |= ENQUEUE_MIGRATED;
    else
#endif
    if (p->in_iowait) {
        delayacct_blkio_end(p);
        atomic_dec(&task_rq(p)->nr_iowait);
    }
    ...
}
```

The `else` before the `if (p->in_iowait)` block ensures the decrement is only done in `ttwu_do_activate()` for non-migrated tasks (since migrated tasks already decremented in `try_to_wake_up()`). This fix is correct because in both cases, the decrement is guaranteed to happen after `schedule()` has completed the increment, either through the `on_cpu` acquire barrier (migration case) or through the runqueue lock (same-CPU case).

## Triggering Conditions

The following conditions are needed to trigger this bug:

- **SMP system with at least 2 CPUs:** The race requires concurrent execution of `schedule()` on one CPU and `try_to_wake_up()` on another CPU for the same task.
- **Kernel version v5.8-rc1 through v5.10-rc4:** The bug was introduced by commit `c6e7bd7afaeb` (merged in v5.8-rc1) and fixed by this commit (merged in v5.10-rc5).
- **Tasks in TASK_UNINTERRUPTIBLE with in_iowait set:** The task being woken must have `p->in_iowait` set, which happens when a task sleeps waiting for I/O (e.g., `io_schedule()`).
- **Tight schedule/wakeup interleaving:** The waking CPU must enter `try_to_wake_up()` and pass the `p->on_rq` check (seeing 0) before the sleeping CPU's `schedule()` has reached the `atomic_inc(&rq->nr_iowait)` line. This requires near-simultaneous execution.
- **`ttwu_queue_wakelist()` path or early decrement:** The bug is most easily triggered when the wakeup goes through the `ttwu_queue_wakelist()` optimization (which requires `p->on_cpu` to still be set and the wakeup to be from a remote CPU), but even the regular path was affected since the decrement was before the `on_cpu` spin.

The bug was originally reported on ARM64 with 96 CPUs running hackbench-process-pipes (which creates many short-lived tasks doing pipe I/O). The higher CPU count and ARM64's weaker memory ordering made the race more likely. However, the bug is architectural-independent — it can occur on x86 as well, just less frequently due to x86's stronger memory model.

Reproduction probability increases with: more CPUs, higher task churn, I/O-heavy workloads (pipe operations, disk I/O), and weaker memory ordering architectures.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why this bug cannot be reproduced with kSTEP

**Kernel version too old:** The bug was introduced in v5.8-rc1 and fixed in v5.10-rc5. kSTEP supports Linux v5.15 and newer only. The buggy code path (the early `atomic_dec(&task_rq(p)->nr_iowait)` in `try_to_wake_up()` before the `on_cpu` synchronization) does not exist in any kernel version that kSTEP supports, because the fix was already applied years before v5.15.

### 2. What would need to be added to kSTEP

Even if kSTEP supported older kernels, reproducing this bug would be challenging because:

- **Race condition timing:** The bug requires precise interleaving between `schedule()` on one CPU and `try_to_wake_up()` on another CPU for the same task. kSTEP's task management APIs (`kstep_task_block()`, `kstep_task_wakeup()`) drive task state transitions, but the critical race occurs within the internal kernel scheduler code paths between `deactivate_task()` and the `nr_iowait` increment. kSTEP cannot inject code execution between these two internal scheduler operations.

- **I/O wait state:** The bug requires `p->in_iowait` to be set. kSTEP does not currently provide an API to put tasks into I/O wait state (e.g., something like `kstep_task_iowait(p)` or `kstep_task_io_schedule(p)`). The `in_iowait` flag is normally set by `io_schedule_prepare()` which is called by block I/O paths. A new API like `kstep_task_io_block(p)` would be needed to set `in_iowait` on the task before blocking it.

- **Observing nr_iowait underflow:** kSTEP would need to read `rq->nr_iowait` at the precise moment it goes negative, between the early decrement and the late increment. The window is extremely short (a few instructions). While KSYM_IMPORT could be used to read `nr_iowait`, catching the transient negative value would require continuous polling or a custom hook at the exact right moment.

### 3. Kernel version constraint

The fix targets v5.10-rc5, which is well before v5.15. Since kSTEP only supports v5.15+, and the fix is already applied in all supported kernel versions, this bug cannot be reproduced.

### 4. Alternative reproduction methods

Outside of kSTEP, the bug can be reproduced on affected kernel versions (v5.8 through v5.10-rc4) using:

- **hackbench-process-pipes:** As described by Mel Gorman, running `hackbench-process-pipes` on a machine with many CPUs (96+ recommended) while overcommitting causes heavy schedule/wakeup contention on I/O-waiting tasks. After hackbench finishes, check `/proc/loadavg` — on buggy kernels, the load average will remain extremely elevated (200+ on a 96-CPU machine) and not drop back to normal within 10 minutes.

- **Pipe-heavy workloads:** Any workload that creates many tasks doing pipe I/O (which uses `io_schedule()`) will exercise the buggy path. The key is to have many tasks simultaneously blocking on and being woken from I/O wait state.

- **ftrace/tracepoints:** Tracing `sched_switch` and `sched_wakeup` events while monitoring `/proc/stat` for iowait anomalies can help confirm the race is occurring.

- **Custom test program:** A program that creates many thread pairs communicating via pipes, with each pair doing tight read/write loops, can reliably trigger the race on affected kernels. The more thread pairs and the more CPUs, the higher the probability of hitting the race window.
