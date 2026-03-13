# Deadline: task_struct Reference Leak on PI Boost Unthrottle

**Commit:** `b58652db66c910c2245f5bee7deca41c12d707b9`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.10
**Buggy since:** v5.10-rc1 (introduced by `feff2e65efd8` "sched/deadline: Unthrottle PI boosted threads while enqueuing")

## Bug Description

When a SCHED_DEADLINE task is throttled (its runtime is exhausted), the kernel arms a replenishment timer (`dl_timer`) via `start_dl_timer()`. This function calls `get_task_struct()` to increment the task's reference count, ensuring the `task_struct` remains valid until the timer callback `dl_task_timer()` fires and calls `put_task_struct()` to release the reference.

A separate code path introduced by commit `feff2e65efd8` handles the case where a throttled SCHED_DEADLINE task receives a priority inheritance (PI) boost via an `rt_mutex`. When `enqueue_task_dl()` detects that a task is both `dl_boosted` and `dl_throttled`, it cancels the replenishment timer using `hrtimer_try_to_cancel()` and clears the throttle flag so the boosted task can run immediately. However, the original code failed to call `put_task_struct()` after successfully canceling the timer, creating a reference count leak.

Every time this code path executes — a PI-boosted SCHED_DEADLINE task being enqueued while throttled — one reference to the `task_struct` is leaked. Over time this leads to unreclaimable memory, as the `task_struct` (approximately 16 KiB) can never be freed. The bug was discovered during stress testing with `stress-ng --cyclic` on Linux-RT kernels, where the scenario of SCHED_DEADLINE tasks contending on `rt_mutex` locks during exit is common.

The bug affects all kernels from v5.10-rc1 through v6.10-rc4 (a span of nearly four years), and was fixed in the v6.10 release cycle.

## Root Cause

The root cause is a missing `put_task_struct()` call in `enqueue_task_dl()` when it cancels a pending replenishment timer for a PI-boosted deadline task.

The reference counting protocol for the deadline replenishment timer works as follows:

1. **Timer arming** in `start_dl_timer()` (line ~1083 in the buggy kernel):
   ```c
   if (!hrtimer_is_queued(timer)) {
       if (!dl_server(dl_se))
           get_task_struct(dl_task_of(dl_se));  // +1 refcount
       hrtimer_start(timer, act, HRTIMER_MODE_ABS_HARD);
   }
   ```

2. **Timer callback** in `dl_task_timer()` (line ~1234):
   ```c
   unlock:
       task_rq_unlock(rq, p, &rf);
       put_task_struct(p);  // -1 refcount (balances the get in start_dl_timer)
       return HRTIMER_NORESTART;
   ```

Under normal operation, every `get_task_struct()` in `start_dl_timer()` is balanced by a `put_task_struct()` in `dl_task_timer()`. But when `enqueue_task_dl()` cancels the timer before it fires, the callback never executes, and the reference is never released.

The buggy code in `enqueue_task_dl()`:
```c
if (is_dl_boosted(&p->dl)) {
    if (p->dl.dl_throttled) {
        hrtimer_try_to_cancel(&p->dl.dl_timer);  // cancel timer
        p->dl.dl_throttled = 0;
        // BUG: no put_task_struct() — the reference from
        // start_dl_timer()'s get_task_struct() is leaked!
    }
}
```

The function `hrtimer_try_to_cancel()` returns:
- `0` if the timer was not active (nothing to cancel)
- `1` if the timer was successfully canceled (was pending)
- `-1` if the timer callback is currently executing (cannot cancel)

When the return value is `1`, the timer was pending and has been removed. Since the timer callback (`dl_task_timer`) will never run, the `put_task_struct()` it would have called will never execute. The original code ignores this return value entirely, leaking the reference in all cases where the timer was successfully canceled.

When the return value is `-1`, the timer callback is already running. In this case, the callback will eventually call `put_task_struct()` itself. The comment in `dl_task_timer()` notes that boosted threads are handled by an early `goto unlock` which still reaches the `put_task_struct()` at the end.

## Consequence

The primary observable consequence is a **memory leak of `task_struct` objects**. Each leaked `task_struct` is approximately 16,136 bytes (as reported by `kmemleak`). In workloads that frequently trigger PI boosting of throttled SCHED_DEADLINE tasks — such as the `stress-ng --cyclic` test that spawns 30 deadline threads doing intensive work with mutex contention during exit — the leak accumulates rapidly.

The `kmemleak` tool reports these as unreferenced objects with a backtrace through `dup_task_struct` → `copy_process` → `kernel_clone`, confirming they are `task_struct` allocations from `fork()` that were never freed because their reference count never reached zero. The leaked memory is permanently unreclaimable until reboot.

On long-running systems (servers, embedded RTOS), this leads to progressively increasing memory consumption. In extreme cases, this could exhaust available memory and trigger OOM kills. The bug is particularly impactful for real-time workloads on Linux-RT kernels, where SCHED_DEADLINE tasks with short periods (e.g., 100 µs) combined with `rt_mutex` contention make the triggering scenario common.

## Fix Summary

The fix modifies `enqueue_task_dl()` to properly handle the return value of `hrtimer_try_to_cancel()`. Specifically, when the timer is successfully canceled (return value `1`) and the deadline entity is not a `dl_server` (i.e., it's a real task, not a server-only scheduling entity), the code now calls `put_task_struct()` to release the reference that `start_dl_timer()` acquired:

```c
if (p->dl.dl_throttled) {
    if (hrtimer_try_to_cancel(&p->dl.dl_timer) == 1 &&
        !dl_server(&p->dl))
        put_task_struct(p);
    p->dl.dl_throttled = 0;
}
```

The `!dl_server(&p->dl)` check is necessary because `start_dl_timer()` only calls `get_task_struct()` for non-server deadline entities. Server entities (`dl_server` is true) do not have a backing `task_struct` in the timer path, so calling `put_task_struct()` for them would be incorrect.

The fix also adds a comment explaining the `-1` return case: when `hrtimer_try_to_cancel()` returns `-1`, the timer callback is currently running on another CPU. In that case, the callback itself will call `put_task_struct()`, so no action is needed here. The `0` return case (timer not active) implies no `get_task_struct()` was done, so no `put_task_struct()` is needed either.

## Triggering Conditions

The following conditions must all be met simultaneously to trigger this bug:

- **SCHED_DEADLINE task with runtime overrun**: A task scheduled under the SCHED_DEADLINE policy must exhaust its runtime budget (i.e., `dl_se->runtime <= 0`). This causes the task to be throttled (`dl_throttled = 1`) and arms the replenishment timer via `start_dl_timer()`, which does `get_task_struct()`. This is easily triggered with aggressive DL parameters (e.g., 10 µs runtime / 100 µs period) combined with any non-trivial work.

- **The throttled task must be blocked on an rt_mutex**: The task must be sleeping while waiting to acquire an `rt_mutex`. This is the mechanism through which PI boosting occurs. During the `exit()` path, tasks commonly take mm-related rt_mutexes (memory management locks), making this a natural occurrence.

- **A third task must PI-boost the throttled task**: Another task (potentially higher priority) must attempt to acquire an `rt_mutex` that the throttled task currently holds. This triggers priority inheritance, setting `p->dl.dl_boosted = 1` on the throttled task.

- **The throttled task must be enqueued while still boosted and before the timer fires**: When the lock holder releases the lock the throttled task was waiting on, `enqueue_task_dl()` is called. At this point, `is_dl_boosted() == true` and `dl_throttled == true`, entering the buggy code path. The replenishment timer must not have fired yet — otherwise `dl_task_timer()` would have already called `put_task_struct()` and the `dl_throttled` flag would have been cleared.

- **Kernel version**: The bug exists in all kernels containing commit `feff2e65efd8` (v5.10-rc1 and later) and not containing the fix `b58652db66c910c2245f5bee7deca41c12d707b9` (v6.10 and later).

No special kernel configuration beyond `CONFIG_SCHED_DEADLINE=y` is required (enabled by default). No particular CPU count or topology is needed. The bug is more likely to trigger on systems with `CONFIG_PREEMPT_RT` (Linux-RT) because rt_mutexes are used more pervasively (spinlocks are converted to rt_mutexes), but it can also occur on mainline kernels where userspace or kernel code explicitly uses rt_mutexes or futexes with PI.

## Reproduce Strategy (kSTEP)

Reproducing this bug with kSTEP requires two minor extensions to the framework, plus a carefully orchestrated sequence of kthread operations. The core idea is to create real SCHED_DEADLINE kthreads that contend on rt_mutexes, trigger runtime overrun and PI boosting, and then detect the leaked reference count.

### Required kSTEP Extensions

1. **SCHED_DEADLINE task API**: Add a function `kstep_kthread_set_dl(struct task_struct *p, u64 runtime_ns, u64 deadline_ns, u64 period_ns)` that calls `sched_setattr_nocheck()` (or `__sched_setscheduler()`) to set SCHED_DEADLINE parameters on a kthread. This follows the same pattern as `kstep_task_fifo()` for RT tasks.

2. **rt_mutex kthread actions**: Add kthread actions for rt_mutex lock and unlock. Specifically, add `kstep_kthread_rtmutex_lock(struct task_struct *p, struct rt_mutex *lock)` and `kstep_kthread_rtmutex_unlock(struct task_struct *p, struct rt_mutex *lock)` that set the kthread's action to acquire/release the given mutex. These follow the existing action pattern in `kmod/kthread.c` (like `do_block_on_wq` and `do_syncwakeup`).

### Driver Setup

1. **Create three kthreads**: Task A, Task B, and Task C. Pin all three to CPU 1 (not CPU 0, which is reserved).
2. **Set SCHED_DEADLINE parameters** on all three:
   - Task B: runtime=10000 (10 µs), deadline=1000000000 (1 second), period=1000000000 (1 second). The very short runtime ensures B quickly overruns its budget. The long period ensures the replenishment timer won't fire during our test window.
   - Tasks A and C: similar parameters with longer runtime to avoid interfering throttles.
3. **Declare two `rt_mutex` objects**: `lock_a` and `lock_b`, initialized with `rt_mutex_init()`.

### Trigger Sequence

1. **Record Task B's initial refcount**: `initial_ref = refcount_read(&taskB->usage)`.

2. **Task A acquires `lock_a`**: Use `kstep_kthread_rtmutex_lock(taskA, &lock_a)`. Wait for the action to complete (A now holds `lock_a`).

3. **Task B acquires `lock_b`**: Use `kstep_kthread_rtmutex_lock(taskB, &lock_b)`. Wait for completion (B now holds `lock_b`).

4. **Task B spins to exhaust DL runtime**: Task B is still running (its default action is `do_spin`). With a 10 µs runtime, it will quickly overrun. Use `kstep_tick_repeat()` or `kstep_sleep()` to let real time pass so the scheduler's `update_curr_dl()` detects the overrun.

5. **Task B tries to acquire `lock_a`**: Use `kstep_kthread_rtmutex_lock(taskB, &lock_a)`. Task B will block because Task A holds `lock_a`. During the dequeue, `update_curr_dl()` notices the runtime overrun, calls `start_dl_timer()` (which does `get_task_struct(B)` and arms the hrtimer), and sets `dl_throttled = 1`.

6. **Task C tries to acquire `lock_b`**: Use `kstep_kthread_rtmutex_lock(taskC, &lock_b)`. Task C blocks because Task B holds `lock_b`. The rt_mutex PI mechanism boosts Task B (sets `dl_boosted = 1`).

7. **Task A releases `lock_a`**: Use `kstep_kthread_rtmutex_unlock(taskA, &lock_a)`. This wakes Task B, calling `enqueue_task_dl()`. At this point, `is_dl_boosted(B) == true` and `B.dl_throttled == true`, triggering the buggy code path that cancels the timer without calling `put_task_struct()`.

8. **Let Task B run and release locks**: Task B acquires `lock_a`, finishes, and we clean up locks.

### Detection

After the sequence completes and all locks are released:

1. **Check Task B's refcount**: `final_ref = refcount_read(&taskB->usage)`.
2. **On the buggy kernel**: `final_ref > initial_ref` (the leaked `get_task_struct` from `start_dl_timer` is never balanced). Report `kstep_fail("task_struct refcount leaked: before=%d after=%d", initial_ref, final_ref)`.
3. **On the fixed kernel**: `final_ref == initial_ref` (the `put_task_struct()` in `enqueue_task_dl()` properly balances). Report `kstep_pass("task_struct refcount balanced")`.

Alternatively, or additionally, import the `dl_throttled` field and the timer state using `KSYM_IMPORT` or direct struct access through `internal.h`, and log the state transitions to confirm the exact code path is exercised:
- Log `p->dl.dl_throttled` before and after enqueue
- Log `hrtimer_is_queued(&p->dl.dl_timer)` to confirm the timer was armed
- Log `is_dl_boosted(&p->dl)` to confirm PI boost

### Expected Behavior

- **Buggy kernel (pre-v6.10)**: Task B's `task_struct` refcount increases by 1 after the sequence and never decreases. Repeated iterations accumulate leaked references. The `kstep_fail` condition triggers.
- **Fixed kernel (v6.10+)**: Task B's `task_struct` refcount returns to its initial value. The `kstep_pass` condition triggers.

### Notes on Timing

The 1-second DL period ensures the replenishment timer is far in the future, giving ample time to execute steps 5-7 before the timer fires. If needed, the period can be increased further. The key timing requirement is that step 7 (Task A releases `lock_a`, triggering B's enqueue) happens before the replenishment timer expires — which is trivially satisfied with a 1-second period.

QEMU should be configured with at least 2 CPUs (CPU 0 for the driver, CPU 1 for the kthreads). No special topology is needed.
