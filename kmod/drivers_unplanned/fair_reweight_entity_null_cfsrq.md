# Fair: NULL pointer dereference in reweight_entity during fork/setpriority race

**Commit:** `13765de8148f71fa795e0a6607de37c49ea5915a`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.17-rc4
**Buggy since:** v5.16-rc1 (introduced by commit `4ef0c5c6b5ba` "kernel/sched: Fix sched_fork() access an invalid sched_task_group")

## Bug Description

A race condition exists between `sched_post_fork()` and `setpriority(PRIO_PGRP)` within a thread group that causes a null pointer dereference in `reweight_entity()` in the CFS scheduler. The scenario involves a main process spawning multiple new threads, which then call `setpriority(PRIO_PGRP, 0, -20)`, wait, and exit. For each new thread, `copy_process()` is invoked, which adds the new `task_struct` to the thread group and then calls `sched_post_fork()`.

The root issue was introduced by commit `4ef0c5c6b5ba`, which moved the `__set_task_cpu()` and `task_fork()` calls from `sched_fork()` (called early in `copy_process()`, before the task is visible to other threads) to `sched_post_fork()` (called after the task has been added to the `thread_group` list). This created a window where a newly forked task is visible in the thread group iteration but has not yet had its per-entity CFS run queue pointer (`se->cfs_rq`) initialized.

During this window, if another thread in the same process group calls `setpriority(PRIO_PGRP, 0, nice)`, the kernel's `__do_sys_setpriority()` iterates over all tasks in the process group via `do_each_pid_thread()`. When it encounters the partially-initialized new task, it calls `set_one_prio()` → `set_user_nice()` → `set_load_weight(p, true)` → `reweight_task()` → `reweight_entity(cfs_rq, se, weight)`. Since the new task's `se->cfs_rq` is NULL (not yet set by `sched_post_fork()`), this dereference crashes the kernel.

This bug was discovered by the Syzkaller fuzzer (`syzbot+af7a719bc92395ee41b3@syzkaller.appspotmail.com`) and was confirmed to be a regression from the `4ef0c5c6b5ba` fix.

## Root Cause

The root cause is a temporal ordering problem introduced by commit `4ef0c5c6b5ba`. That commit moved the initialization of the new task's CPU assignment and CFS run queue pointer from `sched_fork()` to `sched_post_fork()`, creating a window where the task is visible to other threads but has an uninitialized `se->cfs_rq`.

In `copy_process()`, the relevant sequence is:
1. `sched_fork()` is called, which sets `p->__state = TASK_NEW` and initializes basic scheduling properties, but does **not** call `__set_task_cpu()` or `task_fork()` (these were moved to `sched_post_fork()`).
2. The new task is added to the thread group via `list_add_tail_rcu(&p->thread_group, &current->signal->thread_head)`, making it visible to thread group iterators.
3. Later, `sched_post_fork()` is called, which executes `__set_task_cpu(p, smp_processor_id())`. Inside `__set_task_cpu()`, `set_task_rq()` is called, which initializes `se->cfs_rq` (for `CONFIG_FAIR_GROUP_SCHED`) to point to the appropriate per-CPU CFS run queue.

The race occurs between steps 2 and 3. During this window, `se->cfs_rq` is NULL (zero-initialized from `dup_task_struct()`) but the task is visible in the thread group.

When `set_user_nice()` is called on this partially-initialized task, the call chain is:
```
set_user_nice(p, nice)
  → set_load_weight(p, true)         // update_load=true since task is "existing"
    → reweight_task(p, prio)          // called because update_load && fair_sched_class
      → cfs_rq = cfs_rq_of(se)       // returns se->cfs_rq which is NULL
      → reweight_entity(cfs_rq, se, weight)  // cfs_rq is NULL!
        → dequeue_load_avg(cfs_rq, se)       // NULL deref at cfs_rq->avg.load_avg
```

With `CONFIG_FAIR_GROUP_SCHED`, `cfs_rq_of(se)` is defined as `return se->cfs_rq;`, which returns the NULL pointer directly. Without `CONFIG_FAIR_GROUP_SCHED`, `cfs_rq_of(se)` computes the CFS run queue from `task_rq(p)`, which would not be NULL (since `task_rq` uses the CPU number to index the per-CPU run queue array). Therefore, this bug only manifests when `CONFIG_FAIR_GROUP_SCHED` is enabled (which is typical when cgroups are configured).

The old `set_load_weight()` function took a `bool update_load` parameter. Callers in the fork path passed `false` (don't try to reweight on the run queue), while callers in `set_user_nice()` and `__setscheduler_params()` passed `true`. The problem is that `set_user_nice()` always passed `true`, without considering whether the target task might still be in `TASK_NEW` state.

## Consequence

The immediate consequence is a kernel crash (General Protection Fault / NULL pointer dereference) when `reweight_entity()` attempts to dereference the NULL `cfs_rq` pointer. The crash occurs at `dequeue_load_avg(cfs_rq, se)` inside `reweight_entity()`, which accesses `cfs_rq->avg.load_avg` at offset `0xa0` from the NULL pointer.

The crash trace from the Syzkaller report shows:
```
KASAN: null-ptr-deref in range [0x00000000000000a0-0x00000000000000a7]
RIP: 0010:reweight_entity+0x15d/0x440
Call Trace:
  reweight_task+0xde/0x1c0
  set_load_weight+0x21c/0x2b0
  set_user_nice.part.0+0x2d1/0x519
  set_one_prio+0x24f/0x263
  __do_sys_setpriority+0x2d3/0x640
```

Without KASAN, this would be a straight kernel OOPS or panic, killing the affected task or crashing the entire system depending on the `panic_on_oops` setting. On production systems, this could cause denial of service. The bug is triggerable by an unprivileged user since `setpriority(PRIO_PGRP)` and `clone()` are both available to normal users.

The race window is relatively small (between thread_group insertion and `sched_post_fork()`), but Syzkaller demonstrated it is practically exploitable with concurrent thread creation and priority-setting workloads. Any multi-threaded application that both spawns threads and uses `setpriority(PRIO_PGRP)` (e.g., for process-group-wide nice adjustments) could theoretically trigger this.

## Fix Summary

The fix removes the `update_load` parameter from `set_load_weight()` and instead derives the decision internally by checking the task's `TASK_NEW` flag. The new implementation is:

```c
static void set_load_weight(struct task_struct *p)
{
    bool update_load = !(READ_ONCE(p->__state) & TASK_NEW);
    ...
}
```

When `TASK_NEW` is set (indicating the task is still being created and has not yet been fully initialized by `sched_post_fork()`), `update_load` is `false`, and `reweight_task()` is skipped. Instead, the load weight is set directly via `load->weight = scale_load(...)` and `load->inv_weight = ...`, which does not access `cfs_rq` at all.

This is correct because a `TASK_NEW` task is not yet on any run queue, has no valid `cfs_rq` pointer (with `CONFIG_FAIR_GROUP_SCHED`), and does not need its load tracked on a CFS run queue. The direct weight assignment is sufficient since the task's run queue load accounting will be properly established when `sched_post_fork()` completes and the task is first enqueued.

All four call sites are updated to call `set_load_weight(p)` without the `update_load` parameter: `sched_fork()` (where `TASK_NEW` is always set, so `update_load=false` as before), `set_user_nice()` (now safely skips reweight for `TASK_NEW` tasks), `__setscheduler_params()` (same), and `sched_init()` for `init_task` (which is never `TASK_NEW`). The use of `READ_ONCE()` ensures the state read is not torn or cached across the check.

## Triggering Conditions

The following conditions must all be met simultaneously:

1. **Kernel version**: Linux v5.16-rc1 through v5.17-rc3 (kernels containing commit `4ef0c5c6b5ba` but not the fix `13765de8148f`).

2. **CONFIG_FAIR_GROUP_SCHED enabled**: The crash requires `cfs_rq_of(se)` to return `se->cfs_rq` (which is NULL). Without `CONFIG_FAIR_GROUP_SCHED`, `cfs_rq_of()` derives the CFS run queue from the per-CPU run queue, which is always valid.

3. **Multi-threaded process**: A process must be creating new threads (via `clone()` with `CLONE_THREAD`) concurrently with priority changes.

4. **Concurrent `setpriority(PRIO_PGRP)` or `set_user_nice()` call**: Another thread in the same process group must call `setpriority(PRIO_PGRP, 0, nice)`, which iterates all tasks in the process group and calls `set_one_prio()` → `set_user_nice()` on each.

5. **Precise timing**: The `setpriority()` call must encounter the new task after it has been added to the `thread_group` list (via `list_add_tail_rcu()` in `copy_process()`) but before `sched_post_fork()` has been called for it. This is a narrow window of likely a few hundred nanoseconds to a few microseconds, depending on the CPU speed and the amount of work between these two points in `copy_process()`.

6. **Multiple CPUs**: The race requires at least two CPUs — one executing `copy_process()` for the new thread, and another executing `__do_sys_setpriority()` iterating over the thread group. On a single-CPU system, these operations would be serialized.

The reproduction strategy used by Syzkaller involved spawning many threads rapidly while having those threads call `setpriority(PRIO_PGRP, 0, -20)` in a tight loop. The high frequency of concurrent fork+setpriority operations increases the probability of hitting the narrow race window.

## Reproduce Strategy (kSTEP)

This bug **cannot** be cleanly reproduced with kSTEP. It is placed in `drivers_unplanned` for the following reasons:

### 1. Why kSTEP Cannot Reproduce This Bug

The fundamental requirement for triggering this bug is a **race condition between two concurrent userspace syscalls**: `clone()` (thread creation) and `setpriority(PRIO_PGRP)` (group-wide priority change). The race window exists inside the kernel's `copy_process()` function, specifically between the point where the new task is added to the `thread_group` list and the point where `sched_post_fork()` initializes its CFS run queue pointer.

kSTEP cannot reproduce this race for several interconnected reasons:

**a) kSTEP tasks are separate processes, not threads in a shared thread group.** kSTEP creates userspace tasks via `call_usermodehelper()` in `kstep_task_create()`, which spawns independent processes. Even `kstep_task_fork(p, n)` signals an existing task to call `fork()`, creating child processes — not threads sharing a `thread_group` list. The `setpriority(PRIO_PGRP)` syscall iterates over `thread_group`, which only contains threads created with `CLONE_THREAD`. kSTEP has no mechanism to create multiple threads within the same `thread_group`.

**b) The race window is inside `copy_process()` and inaccessible from kSTEP.** The vulnerable window is between `list_add_tail_rcu(&p->thread_group, ...)` and `sched_post_fork(p, kargs)` inside `copy_process()`. This is a narrow section of kernel code executed during a `clone()` syscall. kSTEP's driver runs on CPU 0 and controls task behavior through signals and direct kernel function calls, but it cannot pause or intercept `copy_process()` at the exact point between these two operations. There are no kSTEP callbacks (`on_tick_begin`, `on_sched_softirq_begin`, etc.) that fire during `copy_process()`.

**c) kSTEP's `set_user_nice()` call (via `kstep_task_set_prio`) cannot target a `TASK_NEW` task.** By the time any kSTEP API returns a `task_struct` pointer (from `kstep_task_create()` or observing forked children), the task has already been fully initialized through `sched_post_fork()`, and the `TASK_NEW` flag has been cleared by `wake_up_new_task()`. There is no kSTEP API that provides a handle to a task during its creation window.

**d) The bug is inherently non-deterministic.** Even if a mechanism existed to attempt the race, the window is on the order of hundreds of nanoseconds. kSTEP's smallest time unit (`kstep_sleep()`) operates at tick granularity (milliseconds). Reliably and deterministically hitting a sub-microsecond race window from a kernel module is not feasible, violating kSTEP's requirement for deterministic reproduction.

### 2. What Would Need to Be Added to kSTEP

To reproduce this bug, kSTEP would need several **fundamental** changes:

**a) Thread group creation support.** A new API like `kstep_thread_create(parent)` that creates a new thread sharing the parent's `thread_group` (using `clone()` with `CLONE_THREAD | CLONE_VM | CLONE_SIGHAND`). This requires the parent to be a multi-threaded userspace process with shared signal handling, VM, and file descriptors — a fundamentally different execution model from kSTEP's current independent-process approach.

**b) A hook inside `copy_process()` between thread_group insertion and `sched_post_fork()`.** Something like `on_task_thread_group_added(p)` that fires after `list_add_tail_rcu(&p->thread_group, ...)` but before `sched_post_fork()`. This would require patching `copy_process()` in the kernel source, which is a fundamental modification to the fork path.

**c) Ability to call `set_user_nice()` on tasks during creation.** This exists (`kstep_task_set_prio` calls `set_user_nice()`), but requires a valid `task_struct` pointer to a task in `TASK_NEW` state, which is only available during the race window described above.

### 3. Direct State Manipulation (Not Recommended)

A workaround using direct internal state manipulation would be: (1) create a kthread, (2) manually set `p->__state |= TASK_NEW` and `se->cfs_rq = NULL`, (3) call `set_user_nice()`. On the buggy kernel, this would crash (NULL deref in `reweight_entity`). On the fixed kernel, the `TASK_NEW` check would skip `reweight_task()` and the call would succeed. However, this approach artificially constructs the bug state rather than reproducing the actual race condition, and the kernel crash on the buggy kernel would terminate the QEMU VM without a clean pass/fail result.

### 4. Alternative Reproduction Methods Outside kSTEP

The most reliable reproduction method is to run the Syzkaller reproducer (or a similar C program) directly on the buggy kernel. The program should:
1. Be a multi-threaded process that spawns threads in a tight loop (`clone()` with `CLONE_THREAD`)
2. Have existing threads continuously call `setpriority(PRIO_PGRP, 0, -20)`
3. Run on a multi-CPU system to maximize race window coverage
4. Repeat for many iterations (thousands to millions) to hit the narrow window

The Syzkaller bug ID is `9d9c27adc674e3a7932b22b61c79a02da82cbdc1`. The crash is detected as a GPF/OOPS in `reweight_entity` with the call trace through `set_user_nice` → `set_load_weight` → `reweight_task` → `reweight_entity`.
