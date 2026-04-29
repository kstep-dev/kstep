# RT: Double Enqueue in RT Runqueue Caused by rt_effective_prio Race

**Commit:** `f558c2b834ec27e75d37b1c860c139e7b7c3a8e4`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.14-rc5
**Buggy since:** v4.1-rc4 (commit `0782e63bc6fe` "sched: Handle priority boosted tasks proper in setscheduler()")

## Bug Description

A double-enqueue bug exists in the RT scheduler runqueue lists that is triggered when tasks are concurrently changed between RT and CFS scheduling classes via `sched_setscheduler()` while they are subject to priority inheritance (PI) boosting through RT mutexes. The bug manifests as a `list_add double add` BUG in `enqueue_task_rt()`, which ultimately crashes the kernel with an invalid opcode trap.

The root cause is a TOCTOU (Time-of-Check-Time-of-Use) race in `__sched_setscheduler()`. The function `rt_effective_prio()` is called twice during a single `__sched_setscheduler()` invocation: once directly in `__sched_setscheduler()` to compute the new effective priority (used to decide whether to dequeue/re-enqueue the task), and once again inside `__setscheduler()` to actually set the task's priority and scheduling class. Between these two calls, the priority of the PI top task (`pi_top_task`) can be concurrently changed by another CPU (e.g., via `rt_mutex_setprio()` resolving an inheritance chain), causing the two calls to `rt_effective_prio()` to return different results.

The specific dangerous scenario is: the first call to `rt_effective_prio()` returns an RT priority matching the task's current priority, leading to `DEQUEUE_MOVE` being cleared (the task is considered "unchanged" and not dequeued from the RT list). Then the second call inside `__setscheduler()` returns a CFS (fair) priority due to the concurrent PI change, causing the task's `sched_class` to be set to `fair_sched_class` while the task remains on the RT runqueue list (its `on_list` bit is still set). When the task is later setscheduled back to RT, `enqueue_task_rt()` finds it already on the RT list and triggers a WARNING/BUG.

This bug was reported by Mark Simmons at Red Hat and was observed on PREEMPT_RT SMP systems running a stress test that spawns multiple threads doing short sleep/run cycles while being concurrently switched between RT and fair scheduling classes.

## Root Cause

In the buggy code, `__sched_setscheduler()` computes `newprio` early as a "normal" priority:
```c
int newprio = dl_policy(attr->sched_policy) ? MAX_DL_PRIO - 1 :
              MAX_RT_PRIO - 1 - attr->sched_priority;
```

Then, when `pi` is true (which it is for `sched_setscheduler()` calls), it computes the effective priority considering PI boosting:
```c
new_effective_prio = rt_effective_prio(p, newprio);  // FIRST call
if (new_effective_prio == oldprio)
    queue_flags &= ~DEQUEUE_MOVE;
```

This first call to `rt_effective_prio()` reads `rt_mutex_get_top_task(p)` and computes `min(pi_task->prio, newprio)`. If the result equals `oldprio`, the task is considered unchanged and `DEQUEUE_MOVE` is cleared, meaning the dequeue/enqueue will be a no-op (the task stays where it is on the runqueue).

Later, `__setscheduler()` is called, which internally calls `rt_effective_prio()` a **second** time:
```c
static void __setscheduler(struct rq *rq, struct task_struct *p,
                           const struct sched_attr *attr, bool keep_boost)
{
    __setscheduler_params(p, attr);
    p->prio = normal_prio(p);
    if (keep_boost)
        p->prio = rt_effective_prio(p, p->prio);  // SECOND call
    // ... sets sched_class based on p->prio
}
```

Between the first and second calls, the `pi_top_task`'s priority can change on another CPU. For example:
1. **First call**: `pi_top_task` has RT priority 20. Task's current priority is also 20 (boosted). `rt_effective_prio()` returns 20. Since `new_effective_prio == oldprio`, `DEQUEUE_MOVE` is cleared — the task is not dequeued from the RT runqueue.
2. **Concurrent change**: On another CPU, the PI top task is itself setscheduled to a fair (CFS) priority, so `pi_top_task->prio` changes from an RT value to a CFS value (e.g., 120).
3. **Second call**: `rt_effective_prio()` now sees the updated `pi_top_task->prio = 120`. Since `min(120, newprio)` where `newprio` is a CFS priority results in a CFS priority, `p->prio` is set to a non-RT value, and `p->sched_class` is set to `fair_sched_class`.

The result: the task is now classified as a CFS task (`sched_class = fair_sched_class`) but was never removed from the RT runqueue list (`on_list` is still set in `rt_se`). When the task is eventually scheduled back to RT via another `sched_setscheduler()` call, `enqueue_task_rt()` tries to add it to the RT list again, finds it already present, and triggers the `list_add double add` BUG.

The fundamental error is that two separate invocations of `rt_effective_prio()` read shared mutable state (`pi_top_task->prio`) without any synchronization guaranteeing they see the same value. The `p->pi_lock` held during `__sched_setscheduler()` protects the task's own PI chain, but does not prevent concurrent priority changes to `pi_top_task` from another CPU that has acquired `pi_top_task->pi_lock`.

## Consequence

The immediate consequence is a kernel crash. With `CONFIG_DEBUG_LIST` enabled (as on many distribution kernels), the double list addition is caught by `__list_add_valid()` which triggers:
```
list_add double add: new=ffff9867cb629b40, prev=ffff9867cb629b40,
                     next=ffff98679fc67ca0.
kernel BUG at lib/list_debug.c:31!
invalid opcode: 0000 [#1] PREEMPT_RT SMP PTI
```

Without `CONFIG_DEBUG_LIST`, the corruption of the RT runqueue linked list leads to undefined behavior: potentially infinite loops in the RT scheduler's list traversal during `pick_next_task_rt()`, corrupted RT priority queues, and general scheduler instability leading to system hangs or further crashes.

The stack trace shows the crash occurs in the `__sched_setscheduler()` → `enqueue_task_rt()` call path, invoked from the `sched_setscheduler()` syscall. The bug is particularly concerning on PREEMPT_RT systems where RT scheduling correctness is critical for real-time workloads. Any system running concurrent RT workloads with priority inheritance (i.e., using `rt_mutexes`) and dynamic scheduling policy changes is vulnerable.

## Fix Summary

The fix refactors `__sched_setscheduler()` to call `rt_effective_prio()` exactly **once** and reuse the returned value for both the dequeue/move decision and the actual priority assignment. The old `__setscheduler()` function is eliminated entirely.

Specifically, the fix introduces:
1. **`__normal_prio(policy, rt_prio, nice)`**: A new helper that computes the "normal" (non-boosted) priority directly from policy parameters rather than reading from the task struct. This allows computing the target priority before modifying any task fields.
2. **`__setscheduler_prio(p, prio)`**: A new helper that atomically sets `p->prio` and `p->sched_class` based on a given priority value, consolidating the previously duplicated prio-to-class mapping logic.

In the fixed `__sched_setscheduler()`:
```c
newprio = __normal_prio(policy, attr->sched_priority, attr->sched_nice);
if (pi) {
    newprio = rt_effective_prio(p, newprio);  // Called ONCE
    if (newprio == oldprio)
        queue_flags &= ~DEQUEUE_MOVE;
}
// ... dequeue if needed ...
if (!(attr->sched_flags & SCHED_FLAG_KEEP_PARAMS)) {
    __setscheduler_params(p, attr);
    __setscheduler_prio(p, newprio);  // Reuses the SAME newprio
}
```

This eliminates the TOCTOU race entirely: the effective priority is computed once and used consistently for both the scheduling decision and the actual state mutation. Concurrent PI changes are safe because the PI chain will eventually converge to the correct state through subsequent `rt_mutex_setprio()` calls. The fix also applies the same `__setscheduler_prio()` refactoring to `rt_mutex_setprio()`, consolidating the sched_class assignment logic into a single function for clarity and maintainability.

## Triggering Conditions

- **Scheduling policy changes**: At least two threads must be concurrently calling `sched_setscheduler()` (or `sched_setattr()`) to switch tasks between RT (`SCHED_FIFO`/`SCHED_RR`) and CFS (`SCHED_NORMAL`/`SCHED_OTHER`) classes.
- **Priority inheritance**: The target task of `sched_setscheduler()` must be involved in an RT mutex priority inheritance chain. Specifically, it must hold an `rt_mutex` and be boosted by a higher-priority waiter (the `pi_top_task`).
- **Concurrent PI top task priority change**: The `pi_top_task` (the task providing the priority boost) must have its own priority changed concurrently on another CPU — for example, by another `sched_setscheduler()` call targeting that task, or by another level of the PI chain resolving.
- **SMP system**: At least 2 CPUs are required so that `__sched_setscheduler()` and the concurrent PI change can happen on different CPUs simultaneously.
- **CONFIG_RT_MUTEXES**: Must be enabled (which it is on all PREEMPT_RT kernels and most standard kernels).
- **Timing**: The race window is between the first `rt_effective_prio()` call in `__sched_setscheduler()` and the second call inside `__setscheduler()`. The `pi_top_task->prio` must change in exactly this window.

The original reporter triggered it with a test that spawns multiple threads doing short sleep/run cycles while being concurrently setscheduled between RT and fair class. The probability of hitting the race depends on system load and concurrency; it was observed in stress testing on a multi-CPU PREEMPT_RT system.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Kernel Version Too Old

The fix commit `f558c2b834ec` was merged into **v5.14-rc5**. kSTEP supports Linux **v5.15 and newer only**. The buggy kernel (any version before v5.14-rc5) is older than v5.15. By the time v5.15 was released, this fix was already included, so no v5.15+ kernel contains this bug. There is no kernel version that is both (a) within kSTEP's supported range and (b) still has this bug.

### 2. Additional Practical Difficulties (Even if Version Were Supported)

Even if the version constraint were relaxed, reproducing this bug with kSTEP would face significant challenges:

- **`sched_setscheduler()` syscall**: The bug is triggered by calling `sched_setscheduler()` from userspace. kSTEP does not have a `kstep_task_setscheduler()` API. While a kernel module could call `sched_setscheduler()` or `sched_setattr()` internally, this would require adding a new helper to kSTEP.

- **RT mutex priority inheritance**: The race condition requires a task to be boosted via RT mutex PI. kSTEP has no API for creating RT mutexes or orchestrating priority inheritance chains. The task would need to hold an `rt_mutex` and have another task block on it — this requires `kstep_task_rtmutex_lock()` / `kstep_task_rtmutex_unlock()` primitives that do not exist.

- **Precise timing of a race condition**: The bug requires the `pi_top_task->prio` to change between two specific points within `__sched_setscheduler()`, which is a very narrow window. Deterministic reproduction would require either: (a) a way to inject a delay between the two `rt_effective_prio()` calls, or (b) running the concurrent operations repeatedly until the race hits. Neither approach is well-supported by kSTEP's deterministic tick-based execution model.

### 3. What Would Need to Change in kSTEP

To support this bug, kSTEP would need:
- **Version support for v5.14**: The framework would need to be able to build and boot v5.14 kernels.
- **`kstep_task_setscheduler(p, policy, prio)`**: A new API to call `sched_setscheduler_nocheck()` on a task from within the kernel module.
- **`kstep_rtmutex_create()` / `kstep_rtmutex_lock(p, mutex)` / `kstep_rtmutex_unlock(p, mutex)`**: APIs to create RT mutexes and have kSTEP-managed tasks lock/unlock them, enabling PI boosting.
- **Concurrent cross-CPU execution**: A mechanism to run operations on two CPUs simultaneously with overlapping timing to hit the race window.

### 4. Alternative Reproduction Methods

The bug can be reproduced outside kSTEP by:
- Running the original reproducer: spawn N threads (e.g., 16) doing short `nanosleep()` / compute loops, while another set of threads concurrently calls `sched_setscheduler()` on them alternating between `SCHED_FIFO` (prio 1-99) and `SCHED_NORMAL`.
- Using RT mutexes (`pthread_mutex` with `PTHREAD_PRIO_INHERIT` attribute) to create PI chains between the threads.
- Running on a multi-CPU PREEMPT_RT kernel v5.14-rc4 or earlier.
- Monitoring `dmesg` for the `list_add double add` WARNING/BUG.
