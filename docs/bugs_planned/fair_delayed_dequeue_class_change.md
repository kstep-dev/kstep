# Fair: Sched_delayed Task Not Properly Deactivated on Class Change

**Commit:** `75b6499024a6c1a4ef0288f280534a5c54269076`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.12-rc1
**Buggy since:** v6.12-rc1 (introduced by `2e0199df252a` "sched/fair: Prepare exit/cleanup paths for delayed_dequeue")

## Bug Description

The EEVDF delayed dequeue feature (`DELAY_DEQUEUE`) allows CFS tasks that go to sleep while ineligible to remain on the runqueue with `sched_delayed = 1`. These tasks keep `p->on_rq = TASK_ON_RQ_QUEUED` and stay in the CFS rb-tree until the scheduler picks them and completes the dequeue later. When the deferred dequeue eventually completes via `dequeue_entities()`, it calls `__block_task(rq, p)` to write `p->on_rq = 0` and finalize the blocking.

The bug occurs when a task in the `sched_delayed` state has its scheduling class changed from CFS to another class (e.g., SCHED_FIFO) via `sched_setscheduler()` or `sched_setattr()`. The `switched_from_fair()` handler, invoked during the class change, attempts to clean up the delayed dequeue state but fails to call `__block_task()`. This leaves the task with `p->on_rq != 0` even though it has been dequeued from all runqueues, creating an inconsistent scheduler state.

The bug was reported by Paul E. McKenney and Chen Yu. It affects any scenario where a CFS task that is in the sched_delayed state gets its scheduling policy changed to SCHED_FIFO, SCHED_RR, or SCHED_DEADLINE. This is a common operation in real-time applications that dynamically switch tasks between CFS and RT scheduling classes.

## Root Cause

The root cause is in the `switched_from_fair()` function in `kernel/sched/fair.c`. When `__sched_setscheduler()` changes a task's scheduling class, it performs a dequeue/enqueue cycle:

```c
// In __sched_setscheduler() (kernel/sched/syscalls.c):
queue_flags = DEQUEUE_SAVE | DEQUEUE_MOVE | DEQUEUE_NOCLOCK;
queued = task_on_rq_queued(p);   // true for sched_delayed task
if (queued)
    dequeue_task(rq, p, queue_flags);  // dequeue from CFS
// ... change class via __setscheduler_prio() ...
if (queued)
    enqueue_task(rq, p, queue_flags);  // enqueue to new class (e.g., RT)
check_class_changed(rq, p, prev_class, oldprio);  // calls switched_from_fair()
```

During the first `dequeue_task()`, the CFS dequeue proceeds normally because the flags include `DEQUEUE_SAVE | DEQUEUE_MOVE` but NOT `DEQUEUE_SLEEP`, so `dequeue_entity()` doesn't enter the delayed dequeue path. However, critically, the flags also don't include `DEQUEUE_DELAYED`, so the `sched_delayed` flag on the entity is NOT cleared. After this step, the entity is removed from the CFS rb-tree but `se->sched_delayed` remains 1.

After the class change, `check_class_changed()` calls `switched_from_fair()`, which detects `p->se.sched_delayed == 1` and attempts cleanup. In the buggy code:

```c
static void switched_from_fair(struct rq *rq, struct task_struct *p)
{
    detach_task_cfs_rq(p);
    if (p->se.sched_delayed) {
        dequeue_task(rq, p, DEQUEUE_NOCLOCK | DEQUEUE_SLEEP);
        p->se.sched_delayed = 0;
        p->se.rel_deadline = 0;
        if (sched_feat(DELAY_ZERO) && p->se.vlag > 0)
            p->se.vlag = 0;
        // BUG: Missing __block_task(rq, p) here!
    }
}
```

The `dequeue_task(rq, p, DEQUEUE_NOCLOCK | DEQUEUE_SLEEP)` call now goes through the NEW scheduling class (e.g., `dequeue_task_rt()`), since `p->sched_class` has already been changed by `__setscheduler_prio()`. This successfully removes the task from the RT runqueue. However, `__block_task()` is never invoked, because:

1. The CFS path that normally calls `__block_task()` is `dequeue_entities()` with the `DEQUEUE_DELAYED` flag. This path would set `p->on_rq = 0` via `__block_task()`. But `switched_from_fair()` cannot use `DEQUEUE_DELAYED` because the task has already switched classes.
2. The normal `block_task()` function calls `dequeue_task() && __block_task()`, but `switched_from_fair()` calls `dequeue_task()` directly, not `block_task()`.
3. The RT class's `dequeue_task_rt()` doesn't know about delayed dequeue or `__block_task()` — that's a CFS-specific concern.

The result is that `p->on_rq` retains its value of `TASK_ON_RQ_QUEUED` (1), even though the task has been removed from ALL runqueues. The `__block_task()` function, which is responsible for setting `WRITE_ONCE(p->on_rq, 0)` and incrementing `rq->nr_uninterruptible` for sleeping tasks, is never called.

## Consequence

The most severe consequence is that the task becomes permanently stuck. With `p->on_rq != 0` but the task not actually on any runqueue, subsequent attempts to wake the task via `try_to_wake_up()` will see `task_on_rq_queued(p)` return true and skip the activation path entirely. The wake-up code assumes the task is already queued and just needs its state updated, but since the task is not on any runqueue, it will never be scheduled. The task becomes a zombie that cannot be woken up or run.

Additionally, the `rq->nr_uninterruptible` counter is not incremented (since `__block_task()` was skipped), which causes incorrect load tracking. Subsystems that rely on `p->on_rq` for state inference — including KVM preemption notifiers, perf context-switch events, the process freezer, and RCU tasks quiescent-state detection — will all misclassify this task as runnable when it is in fact blocked.

If the stuck task holds any resources (locks, file descriptors, etc.), those resources will never be released. In the worst case, this can lead to system-wide hangs due to lock dependency chains. The bug is deterministic and will always trigger when a sched_delayed CFS task has its scheduling class changed.

## Fix Summary

The fix extracts the delayed-dequeue cleanup logic into a new helper function `finish_delayed_dequeue_entity()` and adds the missing `__block_task()` call to `switched_from_fair()`.

First, the inline code in `dequeue_entity()` that clears `sched_delayed` and optionally zeroes `vlag` is factored into `finish_delayed_dequeue_entity()`:

```c
static inline void finish_delayed_dequeue_entity(struct sched_entity *se)
{
    se->sched_delayed = 0;
    if (sched_feat(DELAY_ZERO) && se->vlag > 0)
        se->vlag = 0;
}
```

Then `switched_from_fair()` is updated to:

```c
if (p->se.sched_delayed) {
    dequeue_task(rq, p, DEQUEUE_NOCLOCK | DEQUEUE_SLEEP);  // dequeue from new class
    finish_delayed_dequeue_entity(&p->se);                   // clean up sched_delayed
    p->se.rel_deadline = 0;
    __block_task(rq, p);                                     // properly set p->on_rq = 0
}
```

The critical addition is `__block_task(rq, p)`, which performs:
- `WRITE_ONCE(p->on_rq, 0)` — marks the task as properly dequeued
- Increments `rq->nr_uninterruptible` if the task contributes to load
- Handles I/O wait accounting if applicable

This makes the class-change path behave identically to the normal `dequeue_entities()` path with `DEQUEUE_DELAYED`, ensuring that `p->on_rq` is always properly set to 0 when a sched_delayed task is forcibly deactivated during a class switch.

## Triggering Conditions

1. **Kernel version:** The bug exists in kernels with the `DELAY_DEQUEUE` feature, starting from `2e0199df252a` (merged in v6.12-rc1). The `DELAY_DEQUEUE` sched feature must be enabled (it is enabled by default).

2. **Scheduling class change:** A CFS task must have its scheduling class changed to a different class (SCHED_FIFO, SCHED_RR, or SCHED_DEADLINE) while the task is in the `sched_delayed` state. This is done via `sched_setscheduler()`, `sched_setattr()`, or the kernel-internal `sched_setattr_nocheck()`.

3. **Task must be sched_delayed:** The task must be in the sched_delayed state, meaning it attempted to sleep (entered `schedule()` with a sleeping state like `TASK_INTERRUPTIBLE`) while being ineligible in EEVDF. Ineligibility occurs when the task's vruntime is sufficiently far ahead of the CFS runqueue's weighted average vruntime. This naturally happens when a task has been running for a long time relative to other tasks on the same CPU, causing its vruntime to advance beyond the average.

4. **CPU count:** At least 2 CPUs are needed. CPU 0 is reserved for the kSTEP driver, so the target tasks should run on CPU 1 or higher.

5. **Deterministic reproduction:** The bug is deterministic once the preconditions are met. If a task is `sched_delayed` at the moment `sched_setattr_nocheck()` is called, the bug will always trigger. The only non-determinism is in achieving the `sched_delayed` state, which depends on the task being ineligible when it sleeps. This can be reliably triggered by ensuring the task has high vruntime relative to other tasks on the same runqueue.

## Reproduce Strategy (kSTEP)

### Step 1: Create tasks

Create two CFS tasks (A and B) and pin both to CPU 1. Task A will be the target that we put into `sched_delayed` state. Task B exists to ensure that when A sleeps, the CFS runqueue still has a task to compare vruntimes against, and to keep the average vruntime low relative to A's vruntime.

```c
struct task_struct *taskA = kstep_task_create();
struct task_struct *taskB = kstep_task_create();
kstep_task_pin(taskA, 1, 1);
kstep_task_pin(taskB, 1, 1);
```

### Step 2: Run task A to build up vruntime

Wake up only task A first and let it run for many ticks to accumulate a high vruntime. Then wake up task B, which will be placed near `min_vruntime` (much lower than A's vruntime).

```c
kstep_task_wakeup(taskA);
kstep_tick_repeat(50);  // task A accumulates vruntime
kstep_task_wakeup(taskB);  // task B placed at low vruntime
kstep_tick_repeat(5);      // let scheduler stabilize
```

### Step 3: Make task A sched_delayed

Use `kstep_tick_until()` with a predicate that checks if task A's sched_entity is ineligible while A is the current task on the CPU. When A is ineligible and running, pause it — this triggers `schedule()` → `block_task()` → `dequeue_task_fair()` → `dequeue_entity()` with `DEQUEUE_SLEEP`. Since A is ineligible, the `DELAY_DEQUEUE` feature kicks in: `se->sched_delayed = 1`, `dequeue_entity()` returns false, and `__block_task()` is NOT called. Task A remains on the runqueue with `p->on_rq != 0`.

```c
// Predicate: check if taskA is running and ineligible
static void *taskA_ineligible(void) {
    struct sched_entity *se = &taskA->se;
    if (taskA->on_cpu && !kstep_eligible(se))
        return taskA;
    return NULL;
}

kstep_tick_until(taskA_ineligible);
kstep_task_pause(taskA);
kstep_sleep();  // wait for the pause to take effect
```

### Step 4: Verify sched_delayed state

Before switching classes, verify the precondition:

```c
TRACE_INFO("sched_delayed=%d, on_rq=%d", taskA->se.sched_delayed, taskA->on_rq);
if (!taskA->se.sched_delayed) {
    kstep_fail("Task A did not enter sched_delayed state");
    return;
}
```

### Step 5: Switch task A from CFS to SCHED_FIFO

Call `kstep_task_fifo(taskA)`, which internally calls `sched_setattr_nocheck()` → `__sched_setscheduler()` → `check_class_changed()` → `switched_from_fair()`.

```c
kstep_task_fifo(taskA);
```

### Step 6: Check the result

After the class switch, check `taskA->on_rq`:
- **Buggy kernel:** `taskA->on_rq != 0` (TASK_ON_RQ_QUEUED = 1). The task appears queued but is not on any runqueue. `kstep_fail()`.
- **Fixed kernel:** `taskA->on_rq == 0`. The task is properly deactivated. `kstep_pass()`.

```c
TRACE_INFO("After class switch: on_rq=%d, sched_delayed=%d", taskA->on_rq, taskA->se.sched_delayed);
if (taskA->on_rq == 0) {
    kstep_pass("on_rq == 0 after class change (task properly deactivated)");
} else {
    kstep_fail("on_rq == %d after class change (task stuck as ghost-queued)", taskA->on_rq);
}
```

### Step 7: Additional verification (wake-up test)

As a secondary check, attempt to wake up the task and verify it actually runs:

```c
kstep_task_wakeup(taskA);
kstep_tick_repeat(5);
if (taskA->on_cpu || task_on_rq_queued(taskA)) {
    kstep_pass("Task A is runnable after wakeup");
} else {
    kstep_fail("Task A is stuck — wakeup had no effect");
}
```

On the buggy kernel, `try_to_wake_up()` would see `on_rq != 0` and skip the enqueue, leaving the task permanently stuck.

### Callbacks

No special callbacks (`on_tick_begin`, etc.) are strictly required, though `on_tick_begin` could be used to log `taskA->on_rq` and `taskA->se.sched_delayed` on each tick for debugging purposes.

### Expected behavior

- **Buggy kernel (before `75b6499024a6`):** After `kstep_task_fifo()`, `taskA->on_rq` is 1 (TASK_ON_RQ_QUEUED) but the task is not on any runqueue. Subsequent `kstep_task_wakeup()` has no effect. `kstep_fail()` is triggered.
- **Fixed kernel (at or after `75b6499024a6`):** After `kstep_task_fifo()`, `taskA->on_rq` is 0. The task can be successfully woken up and scheduled. `kstep_pass()` is triggered.

### Kernel version guard

The driver should be guarded with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)` since the DELAY_DEQUEUE feature was introduced in v6.12-rc1 (first appearing in the v6.11 development cycle). The `kstep_eligible()` function and `sched_delayed` field are only available in these versions.

### Notes on determinism

The key challenge is reliably getting task A into the `sched_delayed` state. Using `kstep_tick_until()` with an ineligibility predicate ensures that we only pause the task when it is both running and ineligible, guaranteeing that the `DELAY_DEQUEUE` path is taken. This makes the driver deterministic. If the predicate never fires (e.g., A is always eligible), the driver should time out and report an error rather than hang.
