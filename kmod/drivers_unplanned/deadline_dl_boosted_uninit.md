# Deadline: Uninitialized dl_boosted Flag Triggers WARN_ON in setup_new_dl_entity

**Commit:** `ce9bc3b27f2a21a7969b41ffb04df8cf61bd1592`
**Affected files:** kernel/sched/deadline.c
**Fixed in:** v5.8-rc3
**Buggy since:** v3.14-rc1 (commit 2d3d891d3344, "sched/deadline: Add SCHED_DEADLINE inheritance logic")

## Bug Description

When a task is first configured as `SCHED_DEADLINE` via the `sched_setattr()` syscall, the kernel initializes its scheduling parameters through `__dl_clear_params()`. This function is responsible for zeroing out all fields of the `sched_dl_entity` structure, including both the static scheduling parameters (runtime, deadline, period) and the dynamic state flags (throttled, yielded, non_contending, overrun). However, the `dl_boosted` flag — which indicates whether a task's priority has been temporarily elevated through SCHED_DEADLINE priority inheritance via rt-mutexes — was omitted from the initialization in `__dl_clear_params()`.

The `dl_boosted` flag was introduced in commit 2d3d891d3344 as part of the SCHED_DEADLINE priority inheritance logic. This flag is set when a non-SCHED_DEADLINE task (or a SCHED_DEADLINE task with a later deadline) inherits the scheduling parameters of a SCHED_DEADLINE task that is blocked on an rt-mutex held by the inheriting task. The flag was added to the `sched_dl_entity` structure along with corresponding logic in `enqueue_task_dl()` and `rt_mutex_setprio()`, but its initialization was never added to `__dl_clear_params()`.

Because the flag was not explicitly zeroed, its value after `__dl_clear_params()` depends on whatever residual memory contents exist in the `sched_dl_entity` structure. For newly forked tasks or tasks switching to SCHED_DEADLINE for the first time, the `dl_boosted` field could contain a non-zero garbage value. This directly triggers a `WARN_ON(dl_se->dl_boosted)` assertion in `setup_new_dl_entity()`, which is called during the enqueue path when the task is first placed on the deadline runqueue.

This bug was discovered by the syzbot automated testing framework (syzbot+5ac8bac25f95e8b221e7@syzkaller.appspotmail.com), which triggered the warning through a fuzzing-generated `sched_setattr()` syscall sequence. Daniel Wagner also confirmed the issue is easily reproducible on PREEMPT_RT kernels, where the rt-mutex priority inheritance path is exercised more frequently due to sleeping spinlocks being converted to rt-mutexes.

## Root Cause

The root cause is a simple initialization omission in the `__dl_clear_params()` function in `kernel/sched/deadline.c`. This function is called from two code paths:

1. `sched_fork()` → `__dl_clear_params()`: When a new task is forked, to initialize its deadline entity.
2. `__sched_setscheduler()` → `__dl_clear_params()`: When a task's scheduling policy is being changed (e.g., from CFS to SCHED_DEADLINE).

The buggy version of `__dl_clear_params()` initializes the following fields:

```c
void __dl_clear_params(struct task_struct *p)
{
    struct sched_dl_entity *dl_se = &p->dl;

    dl_se->dl_runtime       = 0;
    dl_se->dl_deadline      = 0;
    dl_se->dl_period        = 0;
    dl_se->flags            = 0;
    dl_se->dl_bw            = 0;
    dl_se->dl_density       = 0;

    dl_se->dl_throttled     = 0;
    dl_se->dl_yielded       = 0;
    dl_se->dl_non_contending = 0;
    dl_se->dl_overrun       = 0;
}
```

Note that `dl_se->dl_boosted` is conspicuously absent from this list, while all other boolean state flags (`dl_throttled`, `dl_yielded`, `dl_non_contending`, `dl_overrun`) are properly zeroed.

The problem manifests when the task is subsequently enqueued on the deadline runqueue. The call path is:

1. `__sched_setscheduler()` calls `enqueue_task()` to place the task on the new runqueue.
2. `enqueue_task()` dispatches to `enqueue_task_dl()`.
3. `enqueue_task_dl()` calls `enqueue_dl_entity()`.
4. Inside `enqueue_dl_entity()`, when the `ENQUEUE_RESTORE` flag is set and the task's deadline is in the past, it calls `setup_new_dl_entity()`.
5. `setup_new_dl_entity()` contains `WARN_ON(dl_se->dl_boosted)` at line 663.

The `WARN_ON` exists because `setup_new_dl_entity()` should only be called for tasks that are being newly initialized on the deadline runqueue — a boosted task should never reach this path since its deadline parameters come from the pi-waiter, not from a fresh setup. If `dl_boosted` is non-zero due to uninitialized memory, the WARN_ON fires spuriously, producing a kernel warning (or a panic if `panic_on_warn` is set).

The specific code in `setup_new_dl_entity()` is:

```c
static inline void setup_new_dl_entity(struct sched_dl_entity *dl_se)
{
    struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
    struct rq *rq = rq_of_dl_rq(dl_rq);

    WARN_ON(dl_se->dl_boosted);
    WARN_ON(dl_time_before(rq_clock(rq), dl_se->deadline));
    ...
}
```

## Consequence

The most immediate consequence is a kernel warning (`WARNING`) at `kernel/sched/deadline.c:593`. On systems with `panic_on_warn` enabled (common in testing environments, some production security configurations, and syzbot runs), this warning escalates to a full kernel panic, making the system completely unresponsive. The syzbot report shows exactly this scenario:

```
WARNING: CPU: 0 PID: 6973 at kernel/sched/deadline.c:593 setup_new_dl_entity
Kernel panic - not syncing: panic_on_warn set ...
```

The full call trace shows: `entry_SYSCALL_64` → `SyS_sched_setattr` → `__sched_setscheduler` → `enqueue_task_dl` → `enqueue_dl_entity` → `setup_new_dl_entity`. This means any unprivileged user with the ability to call `sched_setattr()` (on systems where SCHED_DEADLINE is available to non-root users, or from root) can trigger this warning.

Even without `panic_on_warn`, the warning pollutes the kernel log and may indicate that the scheduler is operating with incorrect assumptions about the task's boosting state. While the subsequent scheduling operations may still function correctly (the garbage value of `dl_boosted` is not used in a way that corrupts scheduling decisions in most paths), the warning indicates a code integrity issue. On PREEMPT_RT kernels, Daniel Wagner's testing showed a more severe crash (`invalid opcode: 0000 [#1] PREEMPT_RT SMP`) through the `rt_mutex_setprio()` → `enqueue_task_dl()` path, where the uninitialized `dl_boosted` interacts with the RT priority inheritance mechanism.

## Fix Summary

The fix is a single-line addition to `__dl_clear_params()` that initializes `dl_se->dl_boosted` to 0:

```c
dl_se->dl_boosted       = 0;
```

This line is placed alongside the other flag initializations, between the static parameter block and the existing dynamic state flag initializations. After the fix, `__dl_clear_params()` properly zeroes all fields of the `sched_dl_entity` structure, ensuring that no stale or garbage values remain when a task is configured as SCHED_DEADLINE.

The fix is correct and complete because `dl_boosted` should always be 0 when a task is being freshly initialized: priority inheritance boosting can only occur after a task is actively participating in rt-mutex contention, which requires the task to already be running with a valid scheduling policy. By the time `dl_boosted` would legitimately be set to 1 (through `rt_mutex_setprio()`), the task has already been through proper initialization and enqueue cycles. The `WARN_ON(dl_se->dl_boosted)` in `setup_new_dl_entity()` correctly asserts this invariant — the bug was not in the assertion, but in the missing initialization.

## Triggering Conditions

The bug requires:

- **Kernel version**: Any kernel from v3.14-rc1 (which introduced `dl_boosted`) through v5.8-rc2 (the last version without the fix). The fix shipped in v5.8-rc3.
- **CONFIG_SCHED_DEADLINE**: Must be enabled (it is by default in most kernel configs).
- **sched_setattr() syscall**: A process must call `sched_setattr()` with `SCHED_DEADLINE` policy parameters on a task whose `sched_dl_entity.dl_boosted` field happens to contain a non-zero value from uninitialized memory.
- **Memory state**: The `dl_boosted` field must contain a non-zero garbage value. This depends on what previously occupied that memory location in the `task_struct`. In practice, syzbot was able to trigger this reliably, suggesting it occurs commonly with freshly forked tasks or tasks transitioning from another scheduling class.
- **Enqueue path**: The task must be enqueued via a path that reaches `setup_new_dl_entity()`. This happens when `ENQUEUE_RESTORE` is set and the task's deadline is in the past (relative to the runqueue clock), which is the normal case for a task being initially configured as SCHED_DEADLINE.

On PREEMPT_RT kernels, the bug is more easily triggered because sleeping spinlocks are implemented as rt-mutexes, which means the priority inheritance code path (`rt_mutex_setprio()` → `enqueue_task_dl()`) is exercised during regular mutex contention, not just explicit rt-mutex usage.

The bug is deterministic once the memory conditions are met — it does not involve a race condition. Any single `sched_setattr()` call to set a task to SCHED_DEADLINE can trigger it if the uninitialized field is non-zero.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **Kernel version too old**: The fix was merged in v5.8-rc3, which predates kSTEP's minimum supported kernel version of v5.15. kSTEP supports Linux v5.15 and newer only. The bug existed from v3.14-rc1 through v5.8-rc2. Any kernel that kSTEP can run (v5.15+) already contains this fix, so the buggy code path cannot be exercised.

2. **No sched_setattr() interface**: Even if the kernel version constraint were relaxed, reproducing this bug requires calling `sched_setattr()` (or `sched_setscheduler()`) from userspace to transition a task to SCHED_DEADLINE policy. kSTEP operates through kernel module APIs and cannot intercept or invoke userspace syscalls directly. While kSTEP has `kstep_task_create()` for task management, it does not expose an API to change a task's scheduling class to SCHED_DEADLINE dynamically in the way that triggers the `__sched_setscheduler()` → `__dl_clear_params()` → `enqueue_task_dl()` code path.

3. **Task initialization model**: kSTEP creates tasks via kernel thread interfaces and its own task management API. The `__dl_clear_params()` function is called during `sched_fork()` (task creation) and `__sched_setscheduler()` (policy change). kSTEP tasks are created as kernel threads which are initialized with CFS scheduling class. The `dl_boosted` field in these tasks would be zeroed by `sched_fork()` → `__dl_clear_params()` at creation time (or more precisely, left uninitialized the same way, but never checked because the task never enters the DL class). The bug requires the specific transition from non-DL to DL class via `sched_setattr()`.

4. **Alternative reproduction methods**: The simplest reproduction outside kSTEP would be to run a kernel between v3.14-rc1 and v5.8-rc2 with `panic_on_warn=1` on the kernel command line, then execute a simple C program that calls `sched_setattr()` with `SCHED_DEADLINE` parameters:

```c
#include <sched.h>
#include <sys/syscall.h>
struct sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};
int main() {
    struct sched_attr attr = {
        .size = sizeof(attr),
        .sched_policy = 6, /* SCHED_DEADLINE */
        .sched_runtime = 10000000,  /* 10ms */
        .sched_deadline = 30000000, /* 30ms */
        .sched_period = 30000000,   /* 30ms */
    };
    syscall(SYS_sched_setattr, 0, &attr, 0);
    return 0;
}
```

This would trigger the `WARN_ON(dl_se->dl_boosted)` in `setup_new_dl_entity()` if the uninitialized `dl_boosted` field happens to be non-zero. Running this in a loop on a freshly booted system or after heavy memory activity would increase the likelihood of encountering a non-zero residual value.

5. **What would need to change in kSTEP**: To support this class of bugs, kSTEP would need: (a) support for pre-v5.15 kernels, and (b) an API like `kstep_task_set_dl(p, runtime, deadline, period)` that invokes `__sched_setscheduler()` to transition a task to SCHED_DEADLINE. However, since the kernel version constraint alone disqualifies this bug, these extensions are moot for this particular case.
