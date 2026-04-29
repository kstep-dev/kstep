# Deadline: Erroneous BUG_ON in enqueue_task_dl for Deboosted Tasks

**Commit:** `ddfc710395cccc61247348df9eb18ea50321cbed`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v5.19-rc8
**Buggy since:** v5.10-rc5 (introduced by commit `2279f540ea7d` "sched/deadline: Fix priority inheritance with multiple scheduling classes", which changed `dl_boosted` flag to `is_dl_boosted()` function; the original BUG_ON was added in `64be6f1f5f71` in v3.18-rc2)

## Bug Description

In the SCHED_DEADLINE enqueue path (`enqueue_task_dl()`), a `BUG_ON` condition is logically contradictory and always evaluates to true when reached, causing an unconditional kernel panic. The bug affects the special deboost path for non-SCHED_DEADLINE tasks that were temporarily promoted to the deadline scheduling class via priority inheritance (PI) through `rt_mutex`.

When a non-deadline task (e.g., SCHED_NORMAL or SCHED_FIFO) holds an `rt_mutex` that a SCHED_DEADLINE task tries to acquire, the non-deadline task is PI-boosted to the deadline class. During the subsequent deboost (when the PI boost is removed), the task may enter `enqueue_task_dl()` one final time. At that point, the BUG_ON at line 1704 of `deadline.c` fires unconditionally because its first operand (`!is_dl_boosted(&p->dl)`) is always true in the else-if branch that contains it — since the only way to reach that branch is for `is_dl_boosted()` to have returned false in the preceding if-condition.

The bug was latent in the original code (commit `64be6f1f5f71`, v3.18) but was made actively triggerable by commit `2279f540ea7d` (v5.10-rc5), which replaced the separate `dl_boosted` flag with the `is_dl_boosted()` function based on the `pi_se` pointer. This refactoring changed the semantics of the first condition in `enqueue_task_dl()` from a three-way check (`pi_task && dl_prio(pi_task->normal_prio) && dl_boosted`) to a single check (`is_dl_boosted()`), making the else-if branch only reachable when `is_dl_boosted()` is false — which in turn makes the BUG_ON always fire.

## Root Cause

The root cause is a logically contradictory `BUG_ON` condition in `enqueue_task_dl()` in `kernel/sched/deadline.c`. The relevant code (before the fix) is:

```c
static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
    if (is_dl_boosted(&p->dl)) {
        /* Boosted path: override throttle, cancel timer */
        if (p->dl.dl_throttled) {
            hrtimer_try_to_cancel(&p->dl.dl_timer);
            p->dl.dl_throttled = 0;
        }
    } else if (!dl_prio(p->normal_prio)) {
        /* Deboost path for non-DL tasks */
        p->dl.dl_throttled = 0;
        BUG_ON(!is_dl_boosted(&p->dl) || flags != ENQUEUE_REPLENISH);
        return;
    }
    /* ... normal enqueue ... */
}
```

The `is_dl_boosted()` function (introduced by `2279f540ea7d`) checks `pi_of(dl_se) != dl_se`, which is equivalent to `dl_se->pi_se != dl_se`. When a task is PI-boosted to deadline class, `pi_se` points to the donor's DL entity; when not boosted, `pi_se` points to the task's own DL entity (`&p->dl`).

The logical contradiction:
1. The first `if` branch is entered ONLY when `is_dl_boosted(&p->dl)` returns true.
2. The `else if` branch is entered ONLY when `is_dl_boosted(&p->dl)` returned false.
3. Inside the `else if`, the BUG_ON checks `!is_dl_boosted(&p->dl)` — which is **always true** at this point, because we only reach this code when the task is NOT boosted.
4. Since the BUG_ON condition is `!is_dl_boosted() || flags != ENQUEUE_REPLENISH`, and the first operand is always true, the entire expression is always true regardless of the flags value.
5. Therefore, the BUG_ON **always fires** when the else-if branch is entered.

Before commit `2279f540ea7d`, the code used a separate `dl_boosted` flag and a more complex first condition:
```c
if (pi_task && dl_prio(pi_task->normal_prio) && p->dl.dl_boosted) {
    pi_se = &pi_task->dl;
} else if (!dl_prio(p->normal_prio)) {
    BUG_ON(!p->dl.dl_boosted || flags != ENQUEUE_REPLENISH);
    return;
}
```

In the old code, the first condition could fail even when `dl_boosted` was true (if `pi_task` was NULL or `!dl_prio(pi_task->normal_prio)`). This meant the else-if could be entered with `dl_boosted == true`, and the BUG_ON would NOT fire in that case. After the refactoring, the first condition was simplified to just `is_dl_boosted()`, eliminating all cases where the else-if could be entered with the task still marked as boosted. This made the BUG_ON unconditionally fatal.

Additionally, the BUG_ON used strict equality (`flags != ENQUEUE_REPLENISH`) instead of a bitmask check (`!(flags & ENQUEUE_REPLENISH)`). This would also incorrectly trigger the BUG_ON when ENQUEUE_REPLENISH was present alongside other flags (e.g., `ENQUEUE_REPLENISH | ENQUEUE_RESTORE`), even if the original intent was only to check that REPLENISH was included.

## Consequence

When triggered, the BUG_ON causes an immediate kernel panic via the `BUG()` macro, which executes an invalid instruction (ud2 on x86), generating an invalid opcode exception. This results in a full system crash with a stack trace like:

```
kernel BUG at kernel/sched/deadline.c:1704!
invalid opcode: 0000 [#1] PREEMPT SMP
```

The crash is fatal and non-recoverable — the system must be rebooted. Any in-flight work is lost, and filesystem corruption may result if write barriers are not completed.

The bug is most likely to occur on systems using PREEMPT_RT kernels, where regular spinlocks are converted to `rt_mutex` locks. This greatly increases the frequency of PI boosting because any lock contention (memory management locks, file system locks, timer locks, etc.) can trigger priority inheritance chains involving SCHED_DEADLINE tasks. The `stress-ng --cyclic` workload, which creates aggressive SCHED_DEADLINE tasks that interact with the kernel's locking infrastructure during `exit()`, is known to trigger related DL boosting issues.

Even on non-PREEMPT_RT kernels, the bug can be triggered by any workload that involves SCHED_DEADLINE tasks contending on `rt_mutex` or PI-futex (`PTHREAD_PRIO_INHERIT`) locks with non-deadline tasks. The nested PI scenario (CFS task N2 boosted by DL task D1, then N2 blocks on another lock held by CFS task N1, boosting N1 to DL) is a common occurrence in real-time applications.

## Fix Summary

The fix replaces the unconditionally-fatal `BUG_ON` with a soft diagnostic `printk_deferred_once()` that only checks the meaningful part of the condition (whether `ENQUEUE_REPLENISH` is present as a flag):

```c
// Before (buggy):
BUG_ON(!is_dl_boosted(&p->dl) || flags != ENQUEUE_REPLENISH);

// After (fixed):
if (!(flags & ENQUEUE_REPLENISH))
    printk_deferred_once("sched: DL de-boosted task PID %d: REPLENISH flag missing\n",
                         task_pid_nr(p));
```

The fix makes three important changes:

1. **Removes the `!is_dl_boosted()` check entirely**: This check was logically dead — always true in this branch. Removing it eliminates the unconditional crash.

2. **Changes from strict equality to bitmask check**: `flags != ENQUEUE_REPLENISH` is replaced with `!(flags & ENQUEUE_REPLENISH)`. This correctly checks whether the REPLENISH flag is **present** among the flags, rather than requiring it to be the **only** flag. This allows the deboost path to work correctly when other flags are also set (e.g., `ENQUEUE_RESTORE`, `ENQUEUE_MOVE`).

3. **Downgrades from `BUG_ON` to `printk_deferred_once`**: Instead of crashing the kernel, the fix prints a one-time diagnostic message if the REPLENISH flag is unexpectedly missing. This preserves the ability to detect anomalous conditions during development while keeping the system running in production. The use of `printk_deferred_once` is appropriate because this code runs in scheduler context where regular `printk` might cause issues.

The fix is correct because the else-if branch legitimately handles the deboost scenario: a non-SCHED_DEADLINE task returning from DL class after PI boost removal. Clearing `dl_throttled` and returning early is the correct behavior — the task will be re-enqueued by its original scheduling class after `check_class_changed()` processes the class transition.

## Triggering Conditions

The following conditions must be met to trigger this bug:

- **Kernel version between v5.10-rc5 and v5.19-rc7**: The regression was introduced by commit `2279f540ea7d` (merged in v5.10-rc5) which refactored the PI boosting mechanism. The fix was applied in commit `ddfc710395cc` (v5.19-rc8). Kernels outside this range are not affected.

- **CONFIG_RT_MUTEXES=y**: Required for the `is_dl_boosted()` function to have non-trivial behavior. Without this config, `is_dl_boosted()` always returns false, but PI boosting also cannot occur, so the else-if branch is unreachable. This option is enabled by default on most distributions (needed for PI-futexes).

- **CONFIG_SCHED_DEADLINE=y**: Required for deadline scheduling class. Enabled by default.

- **At least one SCHED_DEADLINE task**: A task must be scheduled with `SCHED_DEADLINE` policy (via `sched_setattr()`) to act as the PI donor that boosts a non-DL task.

- **Non-DL task holds an rt_mutex**: A `SCHED_NORMAL` or `SCHED_FIFO` task must hold an `rt_mutex` (or PI-futex) that a SCHED_DEADLINE task tries to acquire. On PREEMPT_RT kernels, regular spinlocks become rt_mutexes, making this extremely common.

- **PI chain triggers DL boost**: The SCHED_DEADLINE task blocking on the mutex causes the non-DL task to be PI-boosted to the deadline class via `rt_mutex_setprio()`.

- **The boosted task must reach enqueue_task_dl() after deboost**: During the deboost transition (when the PI boost is removed), `enqueue_task_dl()` must be called for the task while it is no longer marked as boosted (`is_dl_boosted()` returns false) and its `normal_prio` is not deadline priority. The exact code path for this is subtle and may involve runtime exhaustion during the boost period, timer interactions, or specific enqueue sequences during the PI chain adjustment.

- **PREEMPT_RT kernel increases likelihood**: On PREEMPT_RT, all spinlocks are rt_mutexes, so any lock contention by a SCHED_DEADLINE task can trigger PI boosting of lock holders. The `stress-ng --cyclic` test (which creates SCHED_DEADLINE threads that take rt_mutexes during `exit()`) is known to exercise these code paths.

- **Multi-CPU system**: At least 2 CPUs are needed for concurrent PI chain processing. The PI boost and deboost operations may occur on different CPUs.

## Reproduce Strategy (kSTEP)

This strategy reproduces the BUG_ON by creating genuine rt_mutex contention between a SCHED_DEADLINE kernel thread and a SCHED_NORMAL kernel thread, producing natural PI boosting and deboosting entirely through public kernel APIs. All scheduler state transitions occur through `rt_mutex_lock`, `rt_mutex_unlock`, and `sched_setattr_nocheck` — the driver never writes to internal scheduler fields such as `pi_se`, `dl_throttled`, or `sched_class`. Internal structures are read only for observation and verification.

### QEMU and Kernel Configuration

Configure QEMU with at least 3 CPUs: CPU 0 for the kSTEP driver, CPU 1 and CPU 2 for test kernel threads. Set the kernel boot parameters `panic_on_oops=1 panic=5` to ensure the `BUG_ON` crash results in a detectable QEMU exit rather than a hung VM. The kernel must be built with `CONFIG_RT_MUTEXES=y` (required for `is_dl_boosted()` and the PI mechanism), `CONFIG_SCHED_DEADLINE=y`, and `CONFIG_SMP=y`. For maximum trigger probability, build the kernel with `CONFIG_PREEMPT_RT=y` (full PREEMPT_RT), which converts all spinlocks to rt_mutexes and dramatically increases the frequency of PI boosting events during normal kernel operations — this is the configuration under which the bug was originally reported. The target kernel version must be between v5.10-rc5 (when `2279f540ea7d` was merged) and v5.19-rc7 (before `ddfc710395cc` was applied).

### Driver Architecture: Custom Kthreads with rt_mutex Operations

The driver creates two kernel threads directly using the kernel's standard `kthread_create()` API (available to all kernel modules via `<linux/kthread.h>`), each with a custom thread function body that includes rt_mutex operations. This avoids requiring any kSTEP framework extensions. The rt_mutex lock/unlock functions and `sched_setattr_nocheck` are imported via `KSYM_IMPORT` since they may not be exported to modules. Coordination between the driver and the kthreads uses atomic flags (readable cross-CPU without sleeping). The driver declares these static variables:

```c
static DEFINE_RT_MUTEX(test_mutex);
static struct task_struct *holder_task;     /* CFS kthread: mutex holder */
static struct task_struct *contender_task;  /* DL kthread: PI donor */

static volatile int holder_acquired = 0;   /* Set by holder after lock */
static volatile int release_holder = 0;    /* Set by driver to signal release */
static volatile int holder_done = 0;       /* Set by holder after unlock */

KSYM_IMPORT(rt_mutex_lock);
KSYM_IMPORT(rt_mutex_unlock);
KSYM_IMPORT(sched_setattr_nocheck);
```

Kthread H (the **holder**, SCHED_NORMAL) has this function body:

```c
static int holder_fn(void *data) {
    KSYM_rt_mutex_lock(&test_mutex);        /* Acquire the mutex */
    smp_store_release(&holder_acquired, 1); /* Signal driver */
    while (!smp_load_acquire(&release_holder))  /* Busy-spin while boosted */
        cpu_relax();
    KSYM_rt_mutex_unlock(&test_mutex);      /* Release → triggers deboost */
    smp_store_release(&holder_done, 1);
    return 0;
}
```

Kthread C (the **contender**, to be set as SCHED_DEADLINE) has this function body:

```c
static int contender_fn(void *data) {
    KSYM_rt_mutex_lock(&test_mutex);        /* Blocks → PI boosts holder */
    KSYM_rt_mutex_unlock(&test_mutex);      /* Immediately release */
    return 0;
}
```

Both thread functions use exclusively public kernel APIs (`rt_mutex_lock`, `rt_mutex_unlock`). No internal scheduler fields are accessed or modified by the kthreads.

An alternative approach uses kSTEP's kthread framework: since `kstep_kthread_create()` builds kthreads with an internal `action` function pointer that the thread loops on, define custom action functions (`do_rt_mutex_lock`, `do_rt_mutex_unlock`) in the driver that call `KSYM_rt_mutex_lock` / `KSYM_rt_mutex_unlock`, and set them on the kthread's internal `struct kstep_kthread` via its `action` and `action_arg` fields. This leverages kSTEP's existing kthread management while enabling custom kernel function execution in the kthread's own context.

### Setup Sequence

1. **Create Kthread H** using `kthread_create(holder_fn, NULL, "kstep_holder")`. Bind it to CPU 1 using `kthread_bind()`. Start it with `wake_up_process()`. H immediately acquires `test_mutex` and busy-spins.

2. **Wait for H to acquire the mutex**: Use `kstep_tick_until()` with a predicate that checks `smp_load_acquire(&holder_acquired)`. Once H holds the mutex, it is spinning on CPU 1 as a SCHED_NORMAL task.

3. **Create Kthread C** using `kthread_create(contender_fn, NULL, "kstep_contender")`. Bind it to CPU 1 (same CPU as H, to ensure direct PI contention on the same runqueue). Before starting C, set it to SCHED_DEADLINE using `KSYM_sched_setattr_nocheck()` with aggressive parameters designed to cause rapid runtime exhaustion:

   ```c
   struct sched_attr attr = {
       .size           = sizeof(struct sched_attr),
       .sched_policy   = SCHED_DEADLINE,
       .sched_runtime  = 5000,    /* 5 µs — very short */
       .sched_deadline = 50000,   /* 50 µs */
       .sched_period   = 50000,   /* 50 µs — implicit deadline */
   };
   KSYM_sched_setattr_nocheck(contender_task, &attr);
   ```

   These tight parameters ensure that when H is PI-boosted to DL class, its inherited DL budget (5 µs) runs out almost immediately — well within a single scheduler tick (~1 ms at HZ=1000).

4. **Start Kthread C** with `wake_up_process()`. C calls `rt_mutex_lock(&test_mutex)`. Since H holds the mutex, C blocks. The kernel's PI mechanism fires automatically: `task_blocks_on_rt_mutex()` → `rt_mutex_adjust_prio_chain()` → `rt_mutex_setprio(H, C)`. This boosts H to SCHED_DEADLINE class with `H->dl.pi_se` pointing to C's DL entity.

### Trigger Sequence and DL Runtime Exhaustion

**Phase 1 — PI Boost Active.** After C blocks on the mutex, H is PI-boosted and continues spinning on CPU 1 as a SCHED_DEADLINE task, inheriting C's DL parameters (5 µs runtime, 50 µs deadline/period). Use `kstep_tick_repeat(200)` to advance approximately 200 scheduler ticks (~200 ms at HZ=1000). During each tick on CPU 1, `scheduler_tick()` → `task_tick_dl()` → `update_curr_dl()` detects that H's DL runtime is exhausted (5 µs runtime is consumed within a fraction of the 1 ms tick interval). The runtime-exceeded path fires: `dl_se->dl_throttled = 1` → `__dequeue_task_dl(rq, curr, 0)` → because `is_dl_boosted(dl_se)` returns TRUE (H is still boosted), the `unlikely(is_dl_boosted(dl_se) || !start_dl_timer(curr))` condition short-circuits and `enqueue_task_dl(rq, curr, ENQUEUE_REPLENISH)` is called directly → inside `enqueue_task_dl`, the first branch (is_dl_boosted TRUE) clears `dl_throttled`, cancels the DL timer via `hrtimer_try_to_cancel()`, and falls through to `enqueue_dl_entity()` with ENQUEUE_REPLENISH → `replenish_dl_entity()` resets `H->dl.deadline = rq_clock + 50µs` and `H->dl.runtime = 5µs`. H is re-enqueued on the DL runqueue and continues spinning. This replenish-on-tick cycle repeats every tick, establishing a steady state of rapid DL runtime exhaustion and replenishment while H remains boosted.

**Phase 2 — Trigger the Deboost.** After sufficient ticks have elapsed, set `WRITE_ONCE(release_holder, 1)` and advance one or more ticks (`kstep_tick()`). H observes the release flag, exits the spin loop, and calls `rt_mutex_unlock(&test_mutex)`. The unlock path invokes `rt_mutex_slowunlock()` → wakes C as the new mutex owner → calls `rt_mutex_adjust_prio()` → `rt_mutex_setprio(H, NULL)` to remove H's PI boost. Inside `rt_mutex_setprio()`, the deboost executes under the rq lock in this order:

1. `dequeue_task(rq, H, DEQUEUE_SAVE|DEQUEUE_MOVE)` → `dequeue_task_dl()` → `update_curr_dl()`. At this point, `H->dl.pi_se` still points to C's DL entity (`is_dl_boosted` = TRUE). If runtime is exceeded (which it almost certainly is, since the 5 µs budget was consumed between the last tick and now), the boosted replenish path fires one final time: throttle → dequeue → is_dl_boosted TRUE → enqueue with REPLENISH → replenish. Then `__dequeue_task_dl(rq, H, flags)` removes H from the DL runqueue.

2. `H->dl.pi_se = &H->dl` — boost marker cleared. `is_dl_boosted()` now returns FALSE.

3. `__setscheduler_prio(H, prio)` — sched_class changed to `fair_sched_class`, prio set to H's normal CFS priority.

4. `enqueue_task(rq, H, queue_flag)` — dispatches through `enqueue_task_fair()` (class is now fair).

5. `check_class_changed()` → `switched_from_dl(rq, H)` → calls `hrtimer_try_to_cancel(&H->dl.dl_timer)` to cancel any pending DL replenishment timer.

**Phase 3 — BUG_ON Trigger via DL Timer Race.** The BUG_ON fires when `enqueue_task_dl()` is entered for H in a state where `is_dl_boosted()` returns false and `!dl_prio(H->normal_prio)` is true. While the rq-lock-serialized deboost sequence above appears airtight, the actual trigger exploits a race between the DL replenishment timer (`dl_task_timer`) and the deboost path. The critical scenario unfolds as follows:

During the replenish cycle in Phase 1, each tick's `update_curr_dl()` detects exhaustion, dequeues H, and — because `is_dl_boosted()` is TRUE — immediately re-enqueues via `enqueue_task_dl()` without starting the DL timer. However, the replenishment inside `enqueue_dl_entity()` calls `replenish_dl_entity()`, which resets the deadline to `rq_clock + dl_deadline`. At the boundary of a tick, if the replenishment timing aligns such that a `dl_check_constrained_dl()` or `update_dl_entity()` path DOES arm the DL timer (e.g., for constrained deadline handling or an edge case in the CBS accounting), that timer runs asynchronously. If `switched_from_dl()` in step 5 encounters `hrtimer_try_to_cancel()` returning -1 (timer callback is currently executing on another CPU), the timer is NOT cancelled. The `dl_task_timer()` callback acquires the rq lock and checks: `dl_task(H)` → examines `dl_prio(H->prio)`. If the timer fires in the narrow window AFTER `pi_se` is cleared (step 2) but BEFORE `__setscheduler_prio()` changes the prio (step 3), `H->prio` is still a DL priority, so `dl_task(H)` returns true. Next, `is_dl_boosted(dl_se)` returns false (pi_se was cleared). The timer proceeds to call `enqueue_task_dl(rq, H, ENQUEUE_REPLENISH)`. Inside `enqueue_task_dl()`, the first branch (`is_dl_boosted()`) is skipped, the else-if (`!dl_prio(H->normal_prio)`) is entered — and the BUG_ON fires.

On PREEMPT_RT kernels, this race window is wider because the rq lock semantics differ and hrtimer callbacks may run in different contexts. Additionally, on PREEMPT_RT, the many spinlock-to-rtmutex conversions create frequent PI boosting/deboosting events throughout the kernel, meaning the DL timer and deboost paths are exercised far more often. This is consistent with the original bug reports occurring under `stress-ng --cyclic` on PREEMPT_RT systems.

### Maximizing Trigger Probability

The race window within `rt_mutex_setprio()` is narrow under standard kernels (a few instructions between pi_se clearing and prio update, all under a single rq lock). To maximize the chance of triggering the BUG_ON:

1. **Use PREEMPT_RT kernel** (`CONFIG_PREEMPT_RT=y`): This is the configuration under which the bug was originally discovered. On PREEMPT_RT, the kthreads' busy-spin loop, kernel memory allocations, and other operations naturally contend on rt_mutexes, creating additional PI boosting/deboosting events beyond the explicitly created one. The different timer and locking behavior under PREEMPT_RT significantly increases the race window.

2. **Iteration loop**: Wrap the entire trigger sequence (create kthreads → acquire → boost → exhaust → release → deboost) in a loop, repeating for many iterations (e.g., 1000+). Between iterations, reinitialize the mutex, reset the coordination flags, and re-create the kthreads. Each iteration is an independent chance to hit the timing window.

3. **Multiple concurrent contention pairs**: Create several pairs of (CFS holder, DL contender) kthreads, each with their own rt_mutex, running on different CPUs (e.g., pairs on CPU 1 and CPU 2). This multiplies the number of deboost events per iteration and increases timer contention across CPUs.

4. **Vary DL parameters**: Across iterations, vary the runtime/period combinations (e.g., 1µs/10µs, 5µs/50µs, 10µs/100µs, 50µs/500µs) to exercise different timer behaviors, CBS accounting edge cases, and `dl_check_constrained_dl()` paths. Constrained deadline parameters (`sched_deadline < sched_period`) are particularly useful for exercising the `dl_check_constrained_dl()` path, which has its own `start_dl_timer()` call that can arm the replenishment timer.

5. **Use constrained deadline tasks**: Set C's DL parameters with `sched_deadline < sched_period` (e.g., `sched_runtime=5000, sched_deadline=30000, sched_period=50000`). This activates the `dl_check_constrained_dl()` code path in `enqueue_task_dl()`, which independently checks deadline vs. rq_clock and may arm the DL timer via `start_dl_timer()`, creating additional opportunities for the timer race.

6. **Pin to separate CPUs**: Pin H to CPU 1 and the DL timer processing to CPU 2 (if possible via IRQ affinity). Cross-CPU timer firing increases the likelihood of the timer callback and `rt_mutex_setprio()` executing concurrently.

### Observation and Verification (Read-Only)

Use the `on_tick_begin` callback to read (never write) internal scheduler state for tracing the reproduction:

```c
KSYM_IMPORT(dl_sched_class);
KSYM_IMPORT(fair_sched_class);

void on_tick_begin(void) {
    struct rq *rq = cpu_rq(1);
    struct task_struct *curr = rq->curr;
    if (curr == holder_task) {
        /* READ-ONLY observation of internal state */
        kstep_pass("H: class=%s boosted=%d throttled=%d rt=%lld dl=%lld",
                   curr->sched_class == KSYM_dl_sched_class ? "DL" :
                   curr->sched_class == KSYM_fair_sched_class ? "CFS" : "other",
                   is_dl_boosted(&curr->dl),
                   curr->dl.dl_throttled,
                   curr->dl.runtime,
                   curr->dl.deadline);
    }
}
```

This tracing confirms: H transitions from CFS → DL (boosted) → CFS (deboosted); DL runtime exhaustion occurs during the boost period; `is_dl_boosted` correctly tracks the PI state; and `dl_throttled` cycles as expected during replenishment. Import `is_dl_boosted` via `KSYM_IMPORT` for the read-only observation. All field accesses are reads — no writes.

### Detection and Pass/Fail Criteria

**Buggy kernel (v5.10-rc5 to v5.19-rc7):** If the BUG_ON fires, it triggers a kernel panic: `kernel BUG at kernel/sched/deadline.c:1704! invalid opcode: 0000 [#1] PREEMPT SMP`. With `panic_on_oops=1 panic=5`, the QEMU VM terminates after 5 seconds. The kSTEP test harness detects this as an abnormal VM exit (non-zero exit code or timeout). The `kstep_fail()` call may not execute since the crash preempts the driver.

**Fixed kernel (v5.19-rc8+):** The `BUG_ON` is replaced with `printk_deferred_once("sched: DL de-boosted task PID %d: REPLENISH flag missing\n", ...)`. The deboost completes normally. After `holder_done` is set, verify:
1. H's scheduling class returned to `fair_sched_class` (read `holder_task->sched_class`).
2. H's `dl_throttled` is 0 (read `holder_task->dl.dl_throttled`).
3. Optionally check `data/logs/latest.log` for the printk warning.

If the test completes without crash and H returned to CFS: `kstep_pass("Deboost completed — BUG_ON not triggered")`.

### Notes

- All rt_mutex operations (`rt_mutex_lock`, `rt_mutex_unlock`), scheduling changes (`sched_setattr_nocheck`), and kthread management (`kthread_create`, `wake_up_process`, `kthread_bind`) are public kernel APIs. They induce scheduler state changes through the kernel's intended interfaces. No scheduler-internal fields (`pi_se`, `dl_throttled`, `sched_class`, `normal_prio`) are written directly by the driver.
- The 5 µs / 50 µs DL parameters are deliberately small. A single scheduler tick (~1 ms at HZ=1000) is 20× the DL period, ensuring runtime exhaustion is detected on every tick while the task is boosted.
- If `rt_mutex_lock` / `rt_mutex_unlock` are not exported to modules, `KSYM_IMPORT` resolves them via `kallsyms_lookup_name`. Similarly for `sched_setattr_nocheck`.
- Disable RT bandwidth throttling with `kstep_sysctl_write("kernel/sched_rt_runtime_us", "%d", -1)` to prevent the global RT bandwidth limiter from interfering with DL task execution during the test.
- QEMU should be configured with at least 3 CPUs. Pin the driver to CPU 0 and test kthreads to CPUs 1-2. The `on_tick_begin` callback provides read-only visibility into the exact state transitions during the boost/deboost cycle.
- For PREEMPT_RT builds, the bug is significantly easier to trigger because spinlock-to-rtmutex conversion creates frequent PI boosting/deboosting throughout the kernel. The `stress-ng --cyclic` workload that originally exposed the bug creates aggressive SCHED_DEADLINE threads that interact with kernel locking during `exit()`, triggering rapid PI chains.
