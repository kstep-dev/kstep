# Core: PSI Inconsistent Task State With Delayed Dequeue

**Commit:** `f5aaff7bfa11fb0b2ee6b8fd7bbc16cfceea2ad3`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.12-rc4
**Buggy since:** v6.12-rc1 (introduced by `152e11f6df29` "sched/fair: Implement delayed dequeue")

## Bug Description

The Pressure Stall Information (PSI) subsystem tracks resource pressure by maintaining per-task flags (`psi_flags`) that record whether a task is running, waiting for I/O, or experiencing memory stalls. When a task goes to sleep (blocks), the PSI accounting must clear the `TSK_RUNNING` flag and, if applicable, set the `TSK_IOWAIT` flag. This state transition is critical for accurate pressure tracking.

The introduction of the DELAY_DEQUEUE feature in commit `152e11f6df29` broke this PSI accounting. DELAY_DEQUEUE is an optimization for the EEVDF scheduler that keeps a blocking task on the runqueue (instead of immediately dequeuing it) when the task has negative lag (i.e., it is not "eligible" — its vruntime is above the average). The task remains queued so it can "burn off" its negative lag through continued virtual time competition, preventing tasks from gaming the system by sleeping right before becoming ineligible.

The problem is that `psi_sched_switch()` in `__schedule()` used `!task_on_rq_queued(prev)` to determine whether the previous task was going to sleep. With DELAY_DEQUEUE, a blocking task can remain queued on the runqueue even though it is logically sleeping. This means `task_on_rq_queued(prev)` returns `true`, causing `psi_sched_switch()` to receive `sleep=false`, and PSI does not clear the `TSK_RUNNING` flag from the task. When the task is later woken up or re-enqueued, PSI attempts to set `TSK_RUNNING` again, but finds it already set, triggering a "psi: inconsistent task state!" error.

This bug was widely reported by users running v6.12-rc kernels, appearing as kernel error messages during boot, particularly during CPU hotplug when `cpuhp` kthreads block and wake frequently. It was also reproduced in QEMU with as few as 2 CPUs.

## Root Cause

The root cause lies in the `__schedule()` function in `kernel/sched/core.c`. The function's logic for communicating task sleep status to PSI was written before DELAY_DEQUEUE existed and assumed that a blocking task would always be dequeued from the runqueue. Specifically, the buggy code was:

```c
psi_sched_switch(prev, next, !task_on_rq_queued(prev));
```

This third argument (`sleep`) tells PSI whether the previous task is going to sleep. Before DELAY_DEQUEUE, this was a correct indicator: if a task blocked, `block_task()` would call `dequeue_task()`, which would remove the task from the runqueue, making `task_on_rq_queued(prev)` return `false` and `!task_on_rq_queued(prev)` return `true`.

With DELAY_DEQUEUE, the code path diverges. When `block_task()` calls `dequeue_task()`, the CFS dequeue path (`dequeue_entity()` in `fair.c`) checks whether the DELAY_DEQUEUE feature is enabled, whether the task is sleeping, and whether it is eligible. If the task is not eligible (its vruntime exceeds the CFS runqueue's average vruntime), then `dequeue_entity()` sets `se->sched_delayed = 1` and returns `false` without actually dequeuing the entity. The task remains on the runqueue with `p->on_rq` still set to `TASK_ON_RQ_QUEUED`.

Back in `block_task()`:
```c
static void block_task(struct rq *rq, struct task_struct *p, int flags)
{
    if (dequeue_task(rq, p, DEQUEUE_SLEEP | flags))
        __block_task(rq, p);
}
```

Since `dequeue_task()` returns false (delayed), `__block_task()` is not called, and the task's `p->on_rq` remains set. Consequently, `task_on_rq_queued(prev)` returns `true`, and `psi_sched_switch()` receives `sleep=false`.

The PSI dequeue path (`psi_dequeue()`) also defers to `psi_sched_switch()` for sleep handling when the task is on the CPU:
```c
if ((flags & DEQUEUE_SLEEP) && (p->psi_flags & TSK_ONCPU))
    return;
```

This deferral assumes `psi_sched_switch()` will correctly handle the TSK_RUNNING cleanup. But since `psi_sched_switch()` receives `sleep=false`, it does not clear TSK_RUNNING:
```c
if (sleep) {
    clear |= TSK_RUNNING;  // This branch is NOT taken!
    if (prev->in_memstall)
        clear |= TSK_MEMSTALL_RUNNING;
    if (prev->in_iowait)
        set |= TSK_IOWAIT;
}
```

The task's `psi_flags` retains `TSK_RUNNING` (value 4) even though the task is logically sleeping.

## Consequence

The immediate observable impact is a kernel error message printed to the kernel log:

```
psi: inconsistent task state! task=<pid>:<comm> cpu=<cpu> psi_flags=4 clear=0 set=4
```

This error is emitted by `psi_flags_change()` in `kernel/sched/psi.c` when it detects that a PSI flag is being set that is already set (or a flag being cleared that is not set). The `psi_flags=4` corresponds to `TSK_RUNNING` (1 << NR_RUNNING = 1 << 2 = 4), and `clear=0 set=4` indicates that the code is attempting to set `TSK_RUNNING` without first clearing it, which should never happen in correct operation.

Once the error fires, the `psi_bug` static variable is set to 1, suppressing further error messages for the remainder of the kernel's uptime. The corrupted PSI state means that the pressure tracking metrics (available via `/proc/pressure/cpu`, `/proc/pressure/memory`, `/proc/pressure/io`) may report inaccurate values. CPU pressure, in particular, may undercount stall time because the system believes the task is still running when it is actually sleeping. This can cause monitoring tools and resource management systems (such as `systemd-oomd`, container orchestrators, or custom PSI-based autoscalers) to make incorrect decisions based on stale pressure data.

While the bug does not cause crashes, hangs, or data corruption in the scheduler itself (scheduling decisions remain correct), the PSI accounting corruption persists and compounds over time. In production environments where PSI metrics are used for resource management, this could lead to under-reaction to actual pressure conditions, potentially allowing OOM situations to develop before corrective action is taken.

## Fix Summary

The fix introduces a new boolean variable `block` in `__schedule()` that explicitly tracks whether the task has entered the blocking path, independent of whether it remains on the runqueue:

```c
bool block = false;
// ...
block_task(rq, prev, flags);
block = true;
// ...
psi_sched_switch(prev, next, block);
```

Instead of inferring sleep status from `task_on_rq_queued(prev)` — which is no longer reliable with DELAY_DEQUEUE — the fix directly records that `block_task()` was called. The `block` variable is set to `true` immediately after `block_task(rq, prev, flags)` executes, regardless of whether `dequeue_task()` actually removed the task from the runqueue or deferred the dequeue.

This fix is correct and complete because: (1) `block_task()` is called if and only if the task is genuinely going to sleep (the `signal_pending_state()` check has already passed, confirming the task should be blocked), and (2) PSI needs to know the task's logical state (sleeping vs. running), not its physical queue state (on vs. off the runqueue). The `block` variable captures exactly the right semantic — "this task has been logically blocked" — which is what PSI needs to properly clear `TSK_RUNNING` and set `TSK_IOWAIT`.

The commit message notes this was extracted by K Prateek Nayak from a larger patch by Peter Zijlstra (referenced as [1] in the commit), which addressed multiple PSI-related issues with delayed dequeue. This specific fix was the minimal change needed to resolve the inconsistent state warning.

## Triggering Conditions

The bug requires the following conditions to be simultaneously satisfied:

1. **Kernel version**: v6.12-rc1 or later (before the fix in v6.12-rc4), with the DELAY_DEQUEUE feature enabled. DELAY_DEQUEUE is a scheduler feature controlled by `sched_feat(DELAY_DEQUEUE)` and is enabled by default.

2. **PSI enabled**: `CONFIG_PSI=y` must be set in the kernel configuration. This is the default in most distribution kernels and is typically enabled.

3. **A CFS task that blocks while ineligible**: A CFS (SCHED_NORMAL) task must go to sleep (set its state to `TASK_INTERRUPTIBLE` or `TASK_UNINTERRUPTIBLE` and call `schedule()`) at a point when it is not eligible in the EEVDF sense — i.e., its `vruntime` exceeds the CFS runqueue's `avg_vruntime`. This is common for tasks that have been running for a significant portion of their time slice, as their vruntime advances beyond the weighted average.

4. **Multiple tasks on the same CPU**: There must be at least one other runnable task on the same CPU's runqueue to establish a meaningful `avg_vruntime` against which eligibility is checked. With only a single task on the queue, the task's vruntime equals the average, making it always eligible.

5. **Subsequent wakeup or re-enqueue**: After the context switch with corrupted PSI state, the sleeping task must be woken up (via `try_to_wake_up()`) or otherwise re-enqueued, triggering `psi_enqueue()` which attempts to set `TSK_RUNNING` and discovers the inconsistency.

The bug is highly likely to trigger during normal system operation because CFS tasks frequently block while ineligible — this is the natural result of a task consuming its time slice and then sleeping. It was reliably triggered during boot on multi-CPU systems due to `cpuhp` (CPU hotplug) kthreads that repeatedly block and wake during CPU bring-up. It was also triggered by `kworker` and `rcu_tasks_trace` threads. The bug manifests deterministically whenever a task blocks with `sched_delayed = 1` (delayed dequeue applied) and is subsequently woken up.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by creating the precise conditions under which a CFS task blocks while ineligible, triggering the delayed dequeue path, and then waking the task to expose the PSI inconsistency.

**Step 1: Setup topology and tasks.**
Configure QEMU with at least 2 CPUs. Create two CFS tasks (task A and task B) and pin both to CPU 1 (not CPU 0, which is reserved for the driver). Both tasks should have the same nice value (default nice 0).

```c
struct task_struct *taskA = kstep_task_create();
struct task_struct *taskB = kstep_task_create();
kstep_task_pin(taskA, 1, 2);  // pin to CPU 1
kstep_task_pin(taskB, 1, 2);  // pin to CPU 1
```

**Step 2: Let task A accumulate vruntime to become ineligible.**
After both tasks are on CPU 1's runqueue, advance the scheduler by ticking multiple times. The scheduler will alternate between A and B, advancing both vruntimes. After sufficient ticks, whichever task is currently running will have its vruntime slightly ahead of the average (since the running task's vruntime advances while the waiting task's does not). At this point, the currently running task is ineligible.

```c
kstep_tick_repeat(20);  // Advance time to let tasks compete
```

**Step 3: Identify the currently running task and verify ineligibility.**
Use `cpu_rq(1)->curr` to identify which task is currently running on CPU 1. Verify that it is ineligible using `kstep_eligible(&curr->se)`. The running task should typically be ineligible (or close to it) after consuming its time quantum. If the running task is still eligible, tick a few more times.

```c
struct rq *rq1 = cpu_rq(1);
struct task_struct *running = rq1->curr;
// running task's vruntime > avg_vruntime → ineligible
```

**Step 4: Record PSI flags before blocking.**
Read the running task's `psi_flags` to establish a baseline. It should have `TSK_RUNNING` (4) and `TSK_ONCPU` set.

```c
unsigned int psi_before = running->psi_flags;
```

**Step 5: Block the running task.**
Use `kstep_task_block()` to cause the running task to enter a sleep state. This calls through to `nanosleep()`, which sets the task state to `TASK_INTERRUPTIBLE | TASK_FREEZABLE` and calls `schedule()`. Inside `__schedule()`, the task's non-zero state triggers the blocking path. Since the task is ineligible, `dequeue_entity()` applies DELAY_DEQUEUE — the task stays on the runqueue with `sched_delayed = 1`.

```c
kstep_task_block(running);  // triggers delayed dequeue
```

**Step 6: Advance the scheduler to complete the context switch.**
Tick or sleep to let the context switch complete. The buggy `psi_sched_switch()` call will pass `sleep=false` because the task is still queued.

```c
kstep_sleep();  // let context switch complete
kstep_tick();   // advance scheduler
```

**Step 7: Verify the PSI state corruption (buggy kernel).**
On the buggy kernel, read the blocked task's `psi_flags`. It should still have `TSK_RUNNING` (4) set, even though the task is sleeping. On a fixed kernel, `TSK_RUNNING` would have been cleared by `psi_sched_switch()`.

```c
unsigned int psi_after_block = running->psi_flags;
// Buggy: psi_after_block & TSK_RUNNING != 0 (TSK_RUNNING still set!)
// Fixed: psi_after_block & TSK_RUNNING == 0 (TSK_RUNNING cleared)
```

**Step 8: Wake the task to trigger the PSI inconsistency error.**
Use `kstep_task_wakeup()` to wake the blocked task. On the buggy kernel, `psi_enqueue()` will attempt to set `TSK_RUNNING` but find it already set, triggering the "psi: inconsistent task state!" error in the kernel log. On the fixed kernel, `TSK_RUNNING` was properly cleared during the context switch, so the wakeup sets it cleanly.

```c
kstep_task_wakeup(running);
kstep_sleep();  // let wakeup complete
```

**Step 9: Detect the bug.**
The primary detection method is to check the task's `psi_flags` after the block-and-switch sequence (step 7). On the buggy kernel, `TSK_RUNNING` (value 4) remains set even though the task is sleeping. The pass/fail criteria:

- **FAIL (bug reproduced):** After blocking and context switching, `running->psi_flags & 4` is non-zero (TSK_RUNNING still set while task is sleeping).
- **PASS (bug not present):** After blocking and context switching, `running->psi_flags & 4` is zero (TSK_RUNNING properly cleared).

Note that `TSK_RUNNING` has value 4 (1 << NR_RUNNING where NR_RUNNING = 2). The value can be verified via `#include <linux/psi_types.h>`.

Additionally, the kernel log will contain "psi: inconsistent task state!" on the buggy kernel when the wakeup in step 8 triggers the redundant flag set. This can serve as a secondary confirmation via dmesg inspection.

**Callbacks**: The `on_tick_begin` callback may be used to monitor the scheduler state at each tick and log vruntime/eligibility information for debugging. However, the core reproduction logic does not require callbacks — it is a straightforward create-tick-block-wake sequence.

**Expected behavior summary:**
- **Buggy kernel (v6.12-rc1 to v6.12-rc3):** After step 5/6, the task's `psi_flags` retains `TSK_RUNNING`. After step 8, the kernel prints "psi: inconsistent task state! task=... psi_flags=4 clear=0 set=4".
- **Fixed kernel (v6.12-rc4+):** After step 5/6, the task's `psi_flags` has `TSK_RUNNING` cleared. After step 8, the wakeup proceeds without PSI errors.
