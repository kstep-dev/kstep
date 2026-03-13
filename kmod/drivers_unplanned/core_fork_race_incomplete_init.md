# Core: Fork Race Exposes Incompletely Initialized Task to Syscalls

**Commit:** `b1e8206582f9d680cff7d04828708c8b6ab32957`
**Affected files:** kernel/sched/core.c, kernel/fork.c, include/linux/sched/task.h
**Fixed in:** v5.17-rc5
**Buggy since:** v5.16-rc1 (introduced by commit `4ef0c5c6b5ba` "kernel/sched: Fix sched_fork() access an invalid sched_task_group")

## Bug Description

During process creation via `fork()`, the kernel's `copy_process()` function must fully initialize a new task's scheduler state before making it visible to other processes. Commit `4ef0c5c6b5ba` attempted to fix a use-after-free race involving `sched_task_group` by moving the `__set_task_cpu()` and `task_fork()` calls out of `sched_fork()` (which runs early in `copy_process()`) and into `sched_post_fork()` (which runs after the task is added to the pidhash via `attach_pid()` and the `tasklist_lock` is released). This created a window where the newly forked task is visible via PID lookup but does not have its runqueue pointer (`cfs_rq`), CPU assignment, or other scheduler state properly initialized.

Any system call that looks up tasks by PID — such as `setpriority(PRIO_PGRP)`, `sched_setscheduler()`, `sched_setattr()`, or `set_user_nice()` — can find this incompletely-initialized task via `find_task_by_vpid()` and then operate on it, encountering NULL pointers or stale data. The specific crash reported by syzbot involved `reweight_entity()` being called on a task whose `cfs_rq` pointer was NULL, because `__set_task_cpu()` had not yet run.

Commit `13765de8148f` ("sched/fair: Fix fault in reweight_entity") attempted to paper over one specific instance of this class of bugs by adding a `TASK_NEW` check inside `set_load_weight()`. Linus Torvalds objected during the pull request review, pointing out that this fix was insufficient — it only addressed one function while the fundamental problem was that *any* scheduler operation on the task could fail. Peter Zijlstra agreed and produced a comprehensive fix.

## Root Cause

The root cause is an ordering problem in `copy_process()`. Before commit `4ef0c5c6b5ba`, the initialization flow was:

1. `sched_fork()` — sets `__state = TASK_NEW`, initializes scheduling class, calls `__set_task_cpu()` and `task_fork()` (which sets up `cfs_rq`, `vruntime`, etc.)
2. `write_lock_irq(&tasklist_lock)` — adds task to pidhash via `attach_pid()`
3. `write_unlock_irq(&tasklist_lock)` — task becomes visible
4. `sched_post_fork()` — only called `uclamp_post_fork()`

After commit `4ef0c5c6b5ba`, the flow changed to:

1. `sched_fork()` — sets `__state = TASK_NEW`, initializes scheduling class, but **no longer** calls `__set_task_cpu()` or `task_fork()`
2. `write_lock_irq(&tasklist_lock)` — adds task to pidhash via `attach_pid()`
3. `write_unlock_irq(&tasklist_lock)` — task becomes visible
4. `sched_post_fork()` — **now** calls `__set_task_cpu()`, `task_fork()`, and sets `sched_task_group`

Between steps 3 and 4, the task is findable via `find_task_by_vpid()` but has:
- No CPU assigned (garbage/inherited CPU value from `dup_task_struct()`)
- No runqueue pointers set up (`se.cfs_rq` is NULL or stale)
- No `sched_task_group` properly cloned (still has parent's possibly-freed pointer)

When another thread in the same process group calls `setpriority(PRIO_PGRP, 0, -20)`, the kernel iterates all tasks in the group via `do_each_pid_thread()` / `while_each_pid_thread()`. For each task, `set_one_prio()` calls `set_user_nice()`, which calls `task_rq_lock()`, `dequeue_task()` / `enqueue_task()`, and `set_load_weight()`. The `set_load_weight()` function (when `update_load` is true) calls `reweight_task()`, which calls `reweight_entity()`. This function dereferences `se->cfs_rq`, which is NULL for the incompletely-initialized task, causing a NULL pointer dereference / GPF.

The partial fix in commit `13765de8148f` changed `set_load_weight()` to check for `TASK_NEW` via `!(READ_ONCE(p->__state) & TASK_NEW)` and skip `reweight_task()` in that case. But this only protects one specific code path — many others (e.g., `dequeue_task()`, `enqueue_task()`, any scheduler class callback) would also fail if they encountered the task in this state.

## Consequence

The most immediate consequence is a kernel NULL pointer dereference (GPF) in `reweight_entity()` when it attempts to access `se->cfs_rq` on the incompletely-initialized task. The syzbot report showed:

```
BUG: unable to handle kernel NULL pointer dereference at 0000000000000000
RIP: 0010:sched_slice+0x84/0xc0

Call Trace:
  task_fork_fair+0x81/0x120
  sched_fork+0x132/0x240
  copy_process.part.5+0x675/0x20e0
  _do_fork+0xcd/0x3b0
  do_syscall_64+0x5d/0x1d0
  entry_SYSCALL_64_after_hwframe+0x65/0xca
```

However, as Linus Torvalds emphasized in his review, this is just one manifestation of a whole class of bugs. Any operation that looks up a task by PID and then interacts with its scheduler state could crash, corrupt data, or produce incorrect results. Potential consequences include:

- **Kernel panic/oops**: NULL dereference in any scheduler function that accesses `cfs_rq`, `rq`, or other runqueue-linked state.
- **Data corruption**: If `__set_task_cpu()` hasn't run, the task might appear to be on a wrong CPU's runqueue structures, leading to list corruption or accounting errors.
- **Use-after-free**: The `sched_task_group` pointer inherited from the parent might reference a freed cgroup, exactly the scenario the original commit `4ef0c5c6b5ba` was trying to fix from the opposite direction.
- **Security implications**: An unprivileged user can trigger this via `fork()` + `setpriority(PRIO_PGRP)` in a multithreaded program, potentially causing a denial of service.

## Fix Summary

The fix introduces a new function `sched_cgroup_fork()` that is called in `copy_process()` *before* the task is made visible (before `write_lock_irq(&tasklist_lock)` and `attach_pid()`), but *after* `cgroup_can_fork()` has pinned the cgroup membership. This function performs all the scheduler initialization that was previously deferred to `sched_post_fork()`:

1. Sets `p->sched_task_group` from the pinned cgroup's css_set (under `CONFIG_CGROUP_SCHED`)
2. Calls `rseq_migrate(p)` 
3. Calls `__set_task_cpu(p, smp_processor_id())` to assign the task to a CPU and set up runqueue pointers
4. Calls `p->sched_class->task_fork(p)` (which for CFS calls `task_fork_fair()`, setting up `vruntime` and `cfs_rq`)

The remaining `sched_post_fork()` is simplified to only call `uclamp_post_fork(p)`, which is safe to run after the task is visible.

Additionally, the fix reverts the `TASK_NEW` check workaround from commit `13765de8148f` in `set_load_weight()`. Instead, `set_load_weight()` takes an explicit `bool update_load` parameter, so callers explicitly declare whether they want `reweight_task()` to run. During `sched_fork()`, it's called with `update_load=false`; in `set_user_nice()`, `__setscheduler_params()`, and `sched_init()` for `init_task`, it's called with the appropriate value. This is cleaner and more robust than checking `TASK_NEW` state, because the `TASK_NEW` flag could be cleared or be unreliable in certain code paths.

The key ordering in `copy_process()` after the fix is:
1. `sched_fork()` — basic scheduler init, `TASK_NEW` state
2. `cgroup_can_fork()` — pins cgroup membership
3. **`sched_cgroup_fork()`** — sets task_group, CPU, runqueue, calls `task_fork()`
4. `attach_pid()` — task becomes visible
5. `sched_post_fork()` — only `uclamp_post_fork()`

This ensures the task is fully initialized from a scheduler perspective before it can be found by any PID-based lookup.

## Triggering Conditions

- **Kernel version**: v5.16-rc1 through v5.17-rc4 (commits `4ef0c5c6b5ba` through one commit before `b1e8206582f9`). The partial fix `13765de8148f` was also present in v5.17-rc4 but was insufficient.
- **Configuration**: Requires `CONFIG_CGROUP_SCHED=y` for the `sched_task_group` aspect, but the NULL `cfs_rq` crash can happen even without cgroups since `__set_task_cpu()` is unconditionally deferred.
- **CPUs**: At least 2 CPUs (needed for concurrent `fork()` and `setpriority()` to race).
- **Workload**: A multithreaded program where:
  - The main thread spawns new threads/processes via `fork()` or `clone()`
  - Other threads in the same process group concurrently call `setpriority(PRIO_PGRP, 0, <new_nice>)` which iterates all tasks in the group
  - The `setpriority()` must execute between the `write_unlock_irq(&tasklist_lock)` in `copy_process()` (which makes the new task visible) and the `sched_post_fork()` call (which initializes the scheduler state)
- **Race window**: The window is narrow — between the `tasklist_lock` release and `sched_post_fork()` in `copy_process()`. Only `proc_fork_connector()` executes in between. With a high fork rate and concurrent `setpriority()` calls, the race is reliably triggerable (syzbot found it via automated fuzzing).
- **Triggering syscall**: `setpriority(PRIO_PGRP)` is the reported trigger, but `sched_setscheduler()`, `sched_setattr()`, or any other syscall using `find_task_by_vpid()` followed by scheduler manipulation could also trigger it.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **Requires real userspace `fork()` syscall**: The bug is in the `copy_process()` code path, which is the core of the `fork()`/`clone()` system call implementation. kSTEP creates tasks via `kstep_task_create()` which calls `kthread_create()` internally — this follows a completely different code path (`kthreadd` → `kernel_clone()` with `CLONE_KERNEL` flags) that does not go through the same `copy_process()` → `sched_post_fork()` sequence as a userspace fork. Specifically, kernel threads have their `sched_task_group` set during `init_idle()` or at thread creation and don't use the same cgroup-fork path.

2. **Requires real userspace `setpriority()` syscall**: The race partner is `setpriority(PRIO_PGRP, 0, nice)` which uses `do_each_pid_thread()` to iterate all tasks in a process group and call `set_one_prio()` on each. kSTEP's `kstep_task_set_prio()` changes a single task's priority directly and does not iterate process groups. More fundamentally, kSTEP tasks (kthreads) do not belong to a process group that can be iterated by `setpriority()`.

3. **Requires PID-based task lookup**: The bug manifests because `find_task_by_vpid()` can return a task that is in the pidhash but not yet scheduler-initialized. kSTEP operates on task pointers directly and never does PID-based lookups. There is no way to observe the "visible but uninitialized" window from a kernel module.

4. **Requires concurrent fork + scheduler manipulation**: kSTEP's `kstep_task_fork()` is a synchronous operation that completes before returning — there is no window during which the task is visible but uninitialized. The real `copy_process()` has multiple phases with lock drops between them, which is where the race window exists.

5. **Cannot intercept copy_process() internals**: Even if kSTEP could trigger a real fork, it cannot insert code between `write_unlock_irq(&tasklist_lock)` and `sched_post_fork()` within `copy_process()` to force the race. The race depends on precise timing of concurrent syscalls on different CPUs.

**What would need to change in kSTEP**: To reproduce this bug, kSTEP would need:
- A `kstep_userspace_fork()` API that triggers a real `do_fork()` / `kernel_clone()` with userspace-like semantics (creating a process with a PID in the pidhash, in a process group).
- A `kstep_setpriority_pgrp(pgrp, nice)` API that calls the actual `setpriority()` codepath with `PRIO_PGRP` to iterate tasks by process group.
- A mechanism to stall `copy_process()` between `write_unlock_irq(&tasklist_lock)` and `sched_post_fork()` to widen the race window — e.g., a tracepoint hook or a kprobe injection point. This is fundamentally impossible without kernel source modifications.

These are fundamental architectural changes (real userspace-like process management), not minor API additions.

**Alternative reproduction methods**: 
- Use the syzbot reproducer: spawn many threads that call `fork()` while other threads call `setpriority(PRIO_PGRP, 0, -20)` in a tight loop. On the buggy kernel, this produces a GPF/NULL pointer dereference within seconds.
- A simple C program: create a process group, have N threads calling `fork()` + `_exit()` in a loop, while M threads call `setpriority(PRIO_PGRP, 0, random_nice)` in a loop. The race is narrow but reproducible with sufficient concurrency and a few seconds of execution.
