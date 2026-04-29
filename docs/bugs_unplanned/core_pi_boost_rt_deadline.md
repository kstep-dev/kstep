# Core: Spurious DL Boosting of RT Task Due to Uninitialized Dynamic Deadline

**Commit:** `740797ce3a124b7dd22b7fb832d87bc8fba1cf6f`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.8-rc3
**Buggy since:** v3.14 (introduced by commit `2d3d891d3344` "sched/deadline: Add SCHED_DEADLINE inheritance logic")

## Bug Description

The Linux kernel's priority inheritance (PI) mechanism in `rt_mutex_setprio()` incorrectly marks a DEADLINE task as `dl_boosted` when the PI donor is actually an RT (real-time SCHED_FIFO/SCHED_RR) task, not a DEADLINE task. This occurs because the code compares the donor task's `dl.deadline` field without first verifying that the donor is actually running at DEADLINE priority. Since RT tasks never initialize their `sched_dl_entity` fields, the `dl.deadline` value is zero by default, causing the comparison `dl_entity_preempt(&pi_task->dl, &p->dl)` to evaluate to true for any non-zero deadline holder.

The scenario is as follows: a DEADLINE task (task A) holds an rt_mutex. An RT task (task B) attempts to acquire the same mutex and blocks. The PI mechanism calls `rt_mutex_setprio()` on task A to potentially boost its priority based on task B's priority. When deciding whether to set `dl_boosted`, the code enters the `if (dl_prio(prio))` branch (since the effective priority remains at DEADLINE level). Inside, it checks `dl_entity_preempt(&pi_task->dl, &p->dl)` where `pi_task` is the RT task B. Because task B's `dl.deadline` is zero (never initialized), `dl_time_before(0, p->dl.deadline)` evaluates to true (0 is before any positive deadline), causing task A to be incorrectly marked as `dl_boosted = 1`.

This was discovered by syzbot (syzkaller automated fuzzer) which triggered a `WARN_ON(dl_se->dl_boosted)` at `kernel/sched/deadline.c:628` inside `setup_new_dl_entity()`. The warning fires because a task that is spuriously marked as boosted later enters `setup_new_dl_entity()`, which should never be called on a boosted task. The syzbot reproducer involved a combination of `sched_setattr()` syscalls that set up the right conditions for an RT task to block on a mutex held by a DEADLINE task.

## Root Cause

The root cause is a missing guard in the `rt_mutex_setprio()` function in `kernel/sched/core.c`. The function handles priority inheritance when a higher-priority task blocks on an rt_mutex held by a lower-priority task. The relevant code block (at approximately line 4534 in the buggy version) is:

```c
if (dl_prio(prio)) {
    if (!dl_prio(p->normal_prio) ||
        (pi_task && dl_entity_preempt(&pi_task->dl, &p->dl))) {
        p->dl.dl_boosted = 1;
        queue_flag |= ENQUEUE_REPLENISH;
    } else
        p->dl.dl_boosted = 0;
    p->sched_class = &dl_sched_class;
}
```

The first sub-condition `!dl_prio(p->normal_prio)` handles the case where the mutex holder (`p`) is not natively a DEADLINE task (e.g., it's an RT or CFS task being boosted to DEADLINE priority). This is correct.

The second sub-condition `(pi_task && dl_entity_preempt(&pi_task->dl, &p->dl))` is meant to handle case 2 from the code comment: when a DEADLINE task blocks on a mutex held by another DEADLINE task and the blocker has an earlier deadline. The function `dl_entity_preempt(a, b)` returns true if `a->deadline < b->deadline` (i.e., `a` would preempt `b`).

The bug is that this second sub-condition does not verify that `pi_task` is actually a DEADLINE task before accessing `pi_task->dl.deadline`. When `pi_task` is an RT task (SCHED_FIFO or SCHED_RR), its `sched_dl_entity` (`pi_task->dl`) was never initialized through normal DEADLINE setup paths. The `dl.deadline` field defaults to zero. Since `dl_time_before()` is effectively `(s64)(a - b) < 0`, comparing deadline 0 against any positive deadline will return true, meaning the RT task's uninitialized deadline appears "earlier" than any real DEADLINE task's deadline.

This causes `dl_boosted` to be set to 1 on the mutex holder even though the donor is an RT task, not a DEADLINE task. The `ENQUEUE_REPLENISH` flag is also incorrectly set. When the task is later re-enqueued and its deadline has expired relative to `rq_clock`, the DEADLINE enqueue path calls `setup_new_dl_entity()`, which hits `WARN_ON(dl_se->dl_boosted)` because boosted tasks should never need a new entity setup — their deadlines are supposed to be managed by the boosting mechanism, not reinitialized.

## Consequence

The immediate observable consequence is a kernel `WARN_ON` triggered at `kernel/sched/deadline.c:628` inside `setup_new_dl_entity()`. On kernels configured with `panic_on_warn` (as in the syzbot testing configuration), this causes a full kernel panic and system reboot. Even without `panic_on_warn`, the warning indicates corrupted scheduler state for the affected task.

Beyond the warning, the incorrect `dl_boosted` flag and `ENQUEUE_REPLENISH` can lead to incorrect DEADLINE replenishment behavior: the task may get its runtime and deadline parameters reset at inappropriate times, potentially violating DEADLINE scheduling guarantees. A task incorrectly marked as boosted may not have its deadline properly tracked, leading to scheduling anomalies where the task runs when it shouldn't or doesn't run when it should.

The syzbot report shows the full stack trace: `enqueue_task_dl+0x22da/0x38a0` → `enqueue_task+0x184/0x390` → `__sched_setscheduler+0xe99/0x2190` → `__x64_sys_sched_setattr+0x1b2/0x2f0`. The crash occurred on kernel 4.20.0-rc2+ running on Google Compute Engine hardware. The secondary effect observed in the syzbot report is a lockdep circular dependency warning involving `console_sem.lock` → `pi_lock` → `rq->lock`, triggered because the `WARN_ON` itself calls `printk()` while holding `rq->lock`, but this is a side effect of the warning handler, not the bug itself.

## Fix Summary

The fix adds a single additional check in the `rt_mutex_setprio()` function. The buggy condition:

```c
(pi_task && dl_entity_preempt(&pi_task->dl, &p->dl))
```

is changed to:

```c
(pi_task && dl_prio(pi_task->prio) &&
 dl_entity_preempt(&pi_task->dl, &p->dl))
```

The added `dl_prio(pi_task->prio)` check verifies that the PI donor task (`pi_task`) is actually running at DEADLINE priority (either natively or because it is itself boosted) before comparing its `dl.deadline` field. The `dl_prio()` macro checks whether the priority value falls in the DEADLINE range (below `MAX_DL_PRIO`). By using `pi_task->prio` (the effective/dynamic priority) rather than `pi_task->normal_prio` (the base priority), the fix correctly handles the transitive boosting case where an RT task has been temporarily boosted to DEADLINE priority through another PI chain — in that case its `dl` fields would be properly initialized and the comparison is valid.

This fix is minimal, correct, and complete. It ensures that `dl_entity_preempt()` is only called when both entities have valid DEADLINE parameters. When the donor is a plain RT task (not boosted to DEADLINE), the condition short-circuits to false, and the code falls through to `p->dl.dl_boosted = 0`, which is the correct behavior — an RT task blocking on a mutex held by a DEADLINE task should not cause the DEADLINE holder to be marked as "boosted" in the DEADLINE sense, because the RT task has no deadline to inherit.

## Triggering Conditions

The bug requires the following specific conditions to trigger:

1. **Two tasks with specific scheduling policies:** One task (the mutex holder) must be running with `SCHED_DEADLINE` policy. A second task (the blocker) must be running with `SCHED_FIFO` or `SCHED_RR` (RT) policy. The RT task must never have been temporarily boosted to DEADLINE priority, so its `dl.deadline` field remains at the default value of 0.

2. **An rt_mutex contention:** The DEADLINE task must hold an rt_mutex. The RT task must attempt to acquire the same mutex and block on it. This triggers the priority inheritance mechanism which calls `rt_mutex_setprio()` on the DEADLINE mutex holder.

3. **Priority ordering:** The effective priority (`prio`) computed by the PI chain for the mutex holder must fall in the DEADLINE range (i.e., `dl_prio(prio)` is true). This happens because the holder is natively a DEADLINE task.

4. **The mutex holder must be a native DEADLINE task:** Specifically, `dl_prio(p->normal_prio)` must be true, so the first sub-condition `!dl_prio(p->normal_prio)` is false. This forces evaluation of the second sub-condition where the bug lies.

5. **The RT blocker's uninitialized deadline must compare as "earlier":** Since `pi_task->dl.deadline` is 0 and the DEADLINE holder's `p->dl.deadline` is some positive value, `dl_entity_preempt(&pi_task->dl, &p->dl)` returns true. This is virtually guaranteed for any DEADLINE task with a non-zero deadline.

The bug is highly reproducible given these conditions. There is no race condition or timing sensitivity — it is a deterministic logic error that triggers whenever an RT task blocks on an rt_mutex held by a DEADLINE task. The syzbot fuzzer was able to trigger it using `sched_setattr()` syscalls to set up the scheduling policies and futex operations to create mutex contention.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **Kernel version too old:** The fix was merged into **v5.8-rc3** (committed 2020-06-28). kSTEP supports Linux v5.15 and newer only. The buggy kernel version (anything from v3.14 through v5.8-rc2) is well below the v5.15 minimum. The `checkout_linux.py` tool would need to check out a kernel older than v5.15 to have the buggy code, which is outside kSTEP's supported range.

2. **Requires rt_mutex contention from userspace:** Even if the version constraint were lifted, reproducing this bug requires two tasks to contend on an `rt_mutex`. In the real kernel, this happens through futex operations (`FUTEX_LOCK_PI`) or other kernel locking primitives that support priority inheritance. kSTEP's task management API (`kstep_task_create`, `kstep_task_block`, etc.) does not provide a mechanism to create rt_mutex contention between tasks. There is no `kstep_task_lock_mutex()` or equivalent API that would cause one task to block on an rt_mutex held by another, thereby triggering the `rt_mutex_setprio()` code path.

3. **Requires specific scheduling policy setup:** The bug needs one task set to `SCHED_DEADLINE` and another to `SCHED_FIFO`/`SCHED_RR`. While kSTEP provides `kstep_task_fifo()` for RT tasks and could potentially set DEADLINE parameters, the core issue is the rt_mutex-based PI chain, which requires actual mutex operations.

4. **What would need to be added to kSTEP:** To support this class of bugs, kSTEP would need:
   - A `kstep_task_deadline(p, runtime, deadline, period)` API to set SCHED_DEADLINE parameters on a task.
   - A PI-aware mutex primitive, e.g., `kstep_rt_mutex_create()`, `kstep_rt_mutex_lock(task, mutex)`, `kstep_rt_mutex_unlock(task, mutex)`, that internally uses the kernel's `rt_mutex` infrastructure to trigger priority inheritance chains.
   - These are **not** minor additions — they require deep integration with the kernel's locking subsystem and careful handling of the blocking/waking state machine.

5. **Alternative reproduction methods:** The bug can be reproduced outside kSTEP by:
   - Running on a kernel between v3.14 and v5.8-rc2.
   - Creating two threads: one with `SCHED_DEADLINE` policy (via `sched_setattr()`) and one with `SCHED_FIFO` policy.
   - Using `FUTEX_LOCK_PI` operations to create a PI-aware mutex. Have the DEADLINE thread acquire the futex lock, then have the RT thread attempt to acquire it, blocking and triggering the PI chain.
   - The syzbot reproducer program (referenced in the report as `syz repro`) automates exactly this sequence using syzkaller's syscall fuzzing framework.
   - Alternatively, the `rtmutex` test module or `libc` PI mutex (`pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT)`) could be used to set up the contention scenario.
