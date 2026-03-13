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

Reproducing this bug with kSTEP requires creating the PI boosting/deboosting scenario using kernel rt_mutex APIs and SCHED_DEADLINE tasks. The approach requires minor kSTEP extensions and careful orchestration of kthread operations.

### Required kSTEP Extensions

1. **SCHED_DEADLINE task API**: Add a function `kstep_kthread_set_dl(struct task_struct *p, u64 runtime_ns, u64 deadline_ns, u64 period_ns)` that calls `sched_setattr_nocheck()` to set SCHED_DEADLINE parameters on a kthread. This follows the same pattern as `kstep_task_fifo()` for RT tasks. The function should import `sched_setattr_nocheck` via `KSYM_IMPORT`.

2. **rt_mutex kthread actions**: Add kthread actions for rt_mutex lock and unlock. Specifically, `kstep_kthread_rtmutex_lock(struct task_struct *p, struct rt_mutex *lock)` and `kstep_kthread_rtmutex_unlock(struct task_struct *p, struct rt_mutex *lock)` that set the kthread's action to acquire/release the given mutex. These follow the existing action pattern in `kmod/kthread.c`.

### Driver Setup

1. **Configure QEMU with at least 2 CPUs** (CPU 0 for driver, CPU 1 for DL tasks).

2. **Create three kthreads**: Task A (CFS, lock holder), Task B (CFS, intermediate), Task D (to be set as SCHED_DEADLINE, PI donor). Pin all to CPU 1.

3. **Set Task D to SCHED_DEADLINE** with aggressive parameters:
   - `dl_runtime = 10000` (10 µs)
   - `dl_deadline = 100000` (100 µs)
   - `dl_period = 100000` (100 µs)

4. **Declare an `rt_mutex`**: `RT_MUTEX(test_mutex)`, initialized with `rt_mutex_init()`.

### Trigger Sequence

The goal is to create a PI chain where a CFS task gets boosted to DL and then deboosted:

1. **Task A acquires `test_mutex`**: Use `kstep_kthread_rtmutex_lock(A, &test_mutex)`. A (CFS) now holds the mutex.

2. **Task D tries to acquire `test_mutex`**: Use `kstep_kthread_rtmutex_lock(D, &test_mutex)`. D (SCHED_DEADLINE) blocks on the mutex. The PI mechanism boosts Task A to deadline class: `rt_mutex_setprio(A, D)` is called, setting `A->dl.pi_se` to D's DL entity, changing A's `sched_class` to `dl_sched_class`, and adding `ENQUEUE_REPLENISH` to the enqueue flags.

3. **Let Task A run as DL**: After boosting, Task A runs with deadline scheduling parameters. Use `kstep_tick_repeat()` to advance time and let Task A consume its DL runtime budget. With the 10 µs runtime, Task A will quickly exhaust its budget, causing `update_curr_dl()` to detect the overrun.

4. **Task A releases `test_mutex`**: Use `kstep_kthread_rtmutex_unlock(A, &test_mutex)`. This triggers the PI deboost: `rt_mutex_setprio(A, NULL)` is called, which:
   - Dequeues A (calls `dequeue_task_dl()` → `update_curr_dl()`)
   - Clears `A->dl.pi_se = &A->dl` (is_dl_boosted becomes false)
   - Changes A's class back to `fair_sched_class`
   - Re-enqueues A via `enqueue_task()` → `enqueue_task_fair()`

   During the dequeue step, `update_curr_dl()` may detect runtime exhaustion and attempt to call `enqueue_task_dl()` directly (bypassing the sched_class callback). The specific path is:
   ```c
   // In update_curr_dl():
   if (dl_runtime_exceeded(dl_se) || dl_se->dl_yielded) {
       dl_se->dl_throttled = 1;
       __dequeue_task_dl(rq, curr, 0);
       if (unlikely(is_dl_boosted(dl_se) || !start_dl_timer(curr)))
           enqueue_task_dl(rq, curr, ENQUEUE_REPLENISH);
   }
   ```

   If `is_dl_boosted()` is false at this point (which can happen if the pi_se was already cleared in an earlier step of the deboost sequence) and `start_dl_timer()` fails (returns 0, e.g., because the computed deadline is in the past), then `enqueue_task_dl()` is called directly. Inside, the first branch (`is_dl_boosted()`) is skipped, and the else-if (`!dl_prio(p->normal_prio)`) is entered — triggering the BUG_ON.

### Alternative Direct Approach

If the natural PI chain doesn't reliably trigger the BUG_ON due to timing constraints, a more direct approach can be used during the initial development stage:

1. **Create a kthread and boost it to DL** via rt_mutex PI as described above.
2. **Manually clear `pi_se`**: After the task is boosted and running as DL, use `KSYM_IMPORT` to access the task's `sched_dl_entity` and set `p->dl.pi_se = &p->dl` directly while holding the rq lock.
3. **Trigger `enqueue_task_dl()`**: Call `enqueue_task_dl()` directly (via `KSYM_IMPORT`) with the task and `ENQUEUE_REPLENISH` flags.
4. **Observe the BUG_ON**: On the buggy kernel, this immediately crashes. On the fixed kernel, it prints the warning and continues.

This direct approach guarantees triggering but manipulates internal state. The refinement stage should attempt to trigger the bug through the natural PI chain.

### Detection

- **Buggy kernel**: The `BUG_ON` fires, causing a kernel panic. The QEMU VM will hang or terminate. kSTEP's test harness should detect the VM crash as a test failure.
- **Fixed kernel**: The `printk_deferred_once` message may appear in `dmesg`/kernel log. The task continues to run normally. The test completes successfully. Check for the warning message: `"sched: DL de-boosted task PID %d: REPLENISH flag missing"` in `data/logs/latest.log`. If no warning appears, the deboost completed cleanly — also a pass.

### Pass/Fail Criteria

- `kstep_fail()`: Kernel panics (BUG_ON triggered on buggy kernel). Detected by VM crash / no response.
- `kstep_pass()`: Test completes without crash. Optionally verify that the deboosted task's `dl_throttled` flag is properly cleared (should be 0 after the deboost path returns).

### Notes

- The 100 µs DL period provides a short enough window for runtime exhaustion. If needed, make the period even shorter or the runtime even smaller (e.g., 1000 ns / 10000 ns) to ensure quick exhaustion.
- QEMU should be configured with at least 2 CPUs. Pin the driver to CPU 0 and test kthreads to CPU 1.
- The `on_tick_begin` callback can be used to inspect task state during ticks, logging `p->dl.dl_throttled`, `is_dl_boosted(&p->dl)`, and `p->sched_class` to trace the exact state transitions.
- Import `enqueue_task_dl`, `start_dl_timer`, `is_dl_boosted`, `dl_prio`, and `rt_mutex_setprio` via `KSYM_IMPORT` for direct inspection and invocation if needed.
- For reliable detection on the buggy kernel, set `panic_on_oops=1` in the QEMU kernel command line to ensure the BUG_ON results in a detectable crash rather than a hung state.
