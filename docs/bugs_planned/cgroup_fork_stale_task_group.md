# Cgroup: Use-After-Free of sched_task_group During Fork

**Commit:** `4ef0c5c6b5ba1f38f0ea1cedad0cad722f00c14a`
**Affected files:** kernel/sched/core.c, kernel/fork.c, include/linux/sched/task.h
**Fixed in:** v5.16-rc1
**Buggy since:** v3.6-rc1 (introduced by commit `8323f26ce342` "sched: Fix race in task_group")

## Bug Description

During process creation via `fork()`, the kernel's `copy_process()` function calls `dup_task_struct()` early on to create a copy of the parent's `task_struct`. This copy includes the `sched_task_group` pointer, which links the task to its CPU cgroup's task group hierarchy. Subsequently, `sched_fork()` is called to initialize the child's scheduler state, and this function accesses the child's `sched_task_group` in two critical places: `__set_task_cpu()` (which resolves the per-CPU `cfs_rq` for the task group) and `task_fork_fair()` → `sched_slice()` (which computes the initial time slice using the task group's CFS run queue).

The bug is a race condition between `copy_process()` and concurrent cgroup migration of the parent task. Between the time `dup_task_struct()` copies the parent's `sched_task_group` to the child, and the time `sched_fork()` accesses it, another CPU can migrate the parent to a different cgroup and trigger destruction of the old cgroup. Since the child's `sched_task_group` still points to the old (now-freed) cgroup's task group, `sched_fork()` dereferences freed memory.

The race window exists because, in the buggy code, `sched_fork()` is called before `cgroup_can_fork()`. The function `cgroup_can_fork()` pins the cgroup membership (preventing migration), but it runs later in `copy_process()`. Therefore, during the entire window from `dup_task_struct()` through `sched_fork()`, the parent's cgroup membership can change freely, invalidating the child's copied `sched_task_group` pointer.

This bug was reported from a production environment running the `ebizzy` benchmark, which performs many concurrent forks. The crash manifested as a NULL pointer dereference in `sched_slice()` when accessing the freed task group's `cfs_rq`.

## Root Cause

The root cause is an incorrect ordering of scheduler initialization relative to cgroup membership locking in `copy_process()`. In the buggy kernel, the flow is:

```
copy_process():
  dup_task_struct()          // (1) child->sched_task_group = parent->sched_task_group
  ...
  sched_fork()               // (2) accesses child->sched_task_group
    __set_task_cpu(p, cpu)   //     resolves cfs_rq via task_group->cfs_rq[cpu]
    task_fork_fair(p)        //     calls sched_slice() which walks task_group hierarchy
  ...
  cgroup_can_fork()          // (3) pins cgroup membership — too late!
  ...
  sched_post_fork()
  cgroup_post_fork()
```

The critical problem is at step (2). The `__set_task_cpu()` function performs `set_task_rq(p, cpu)` which resolves the child's `se.cfs_rq` and `se.parent` pointers by walking the `sched_task_group->cfs_rq[cpu]` array. If the task group has been freed, this array is garbage. Similarly, `task_fork_fair()` calls `sched_slice()`, which accesses `cfs_rq->nr_running` and walks the task group hierarchy via `for_each_sched_entity(se)` to compute the time slice. Each step dereferences the freed task group's internal pointers.

The race interleaving that triggers the bug is:

1. **CPU A** (parent): Enters `copy_process()`, calls `dup_task_struct()`. The child's `sched_task_group` now points to cgroup X's `task_group`.
2. **CPU B** (another process): Migrates the parent from cgroup X to cgroup Y via `cgroup_attach_task()` → `cgroup_migrate()`. The parent's `sched_task_group` is updated to point to cgroup Y's `task_group`.
3. **CPU B** or **timer/RCU**: Cgroup X has no more tasks. If cgroup X is rmdir'd (or its reference count drops to zero), `css_free_rwork_fn()` → `sched_unregister_group_rcu()` → `unregister_fair_sched_group()` frees cgroup X's `task_group`, including all its per-CPU `cfs_rq` arrays.
4. **CPU A** (parent): Continues `copy_process()`, enters `sched_fork()`. `__set_task_cpu()` accesses `child->sched_task_group->cfs_rq[smp_processor_id()]` — this is a use-after-free. `task_fork_fair()` → `sched_slice()` further dereferences the freed `cfs_rq`, causing a NULL pointer dereference or accessing corrupted memory.

The old comment in `sched_fork()` before the code that was removed said: "The child is not yet in the pid-hash so no cgroup attach races, and the cgroup is pinned to this child due to cgroup_fork() is ran before sched_fork()." This comment was incorrect because `cgroup_fork()` only copies the parent's `css_set` reference — it does NOT prevent the parent from being migrated to a different cgroup. The actual protection against cgroup migration comes from `cgroup_can_fork()`, which acquires `cgroup_threadgroup_rwsem`, but this runs after `sched_fork()`.

## Consequence

The most severe consequence is a kernel panic due to a NULL pointer dereference (or use-after-free) in `sched_slice()`. The crash trace from the production report shows:

```
BUG: unable to handle kernel NULL pointer dereference at 0000000000000000
PGD 8000001fa0a86067 P4D 8000001fa0a86067 PUD 2029955067 PMD 0
Oops: 0000 [#1] SMP PTI
CPU: 7 PID: 648398 Comm: ebizzy Kdump: loaded Tainted: G  OE  4.18.0.x86_64+
RIP: 0010:sched_slice+0x84/0xc0

Call Trace:
  task_fork_fair+0x81/0x120
  sched_fork+0x132/0x240
  copy_process.part.5+0x675/0x20e0
  _do_fork+0xcd/0x3b0
  do_syscall_64+0x5d/0x1d0
  entry_SYSCALL_64_after_hwframe+0x65/0xca
```

The NULL dereference occurs because after the task group is freed, the `cfs_rq` pointer obtained from `tg->cfs_rq[cpu]` is either NULL (if the memory was zeroed) or a dangling pointer (if the memory was reused). In the reported case, it was NULL, leading to an immediate oops.

Beyond the crash, even without memory being freed (i.e., if the old cgroup still exists but the parent has been migrated), the bug causes incorrect scheduling initialization: the child task's initial `vruntime` and time slice are computed using the wrong cgroup's `cfs_rq` parameters (wrong `min_vruntime`, wrong `nr_running`). This leads to unfair scheduling for the child's first execution quantum, though the child would eventually be placed in the correct cgroup by `cgroup_post_fork()`.

The bug is most likely to manifest on systems with frequent fork activity (web servers, build systems, benchmark suites like `ebizzy`) combined with concurrent cgroup management (container orchestration, systemd cgroup operations). The race window is narrow but non-negligible on multi-core systems under high fork+migration load.

## Fix Summary

The fix moves the dangerous scheduler operations — `__set_task_cpu()` and `task_fork_fair()` — from `sched_fork()` to `sched_post_fork()`, placing them after `cgroup_can_fork()` has pinned the cgroup membership. In `sched_post_fork()`, the child's `sched_task_group` is explicitly re-derived from the `kernel_clone_args`'s `cset` (CSS set), which was determined during `cgroup_can_fork()` and is guaranteed to be valid.

Specifically, in the new `sched_post_fork()`:

```c
void sched_post_fork(struct task_struct *p, struct kernel_clone_args *kargs)
{
    unsigned long flags;
    raw_spin_lock_irqsave(&p->pi_lock, flags);
#ifdef CONFIG_CGROUP_SCHED
    struct task_group *tg;
    tg = container_of(kargs->cset->subsys[cpu_cgrp_id],
                      struct task_group, css);
    p->sched_task_group = autogroup_task_group(p, tg);
#endif
    rseq_migrate(p);
    __set_task_cpu(p, smp_processor_id());
    if (p->sched_class->task_fork)
        p->sched_class->task_fork(p);
    raw_spin_unlock_irqrestore(&p->pi_lock, flags);
    uclamp_post_fork(p);
}
```

The key insight is that `kargs->cset` is set by `cgroup_can_fork()` and represents the definitive cgroup set for the child. By extracting the `task_group` from `kargs->cset->subsys[cpu_cgrp_id]` (passing it through `autogroup_task_group()` for autogroup support), the fix ensures the child's `sched_task_group` is always valid and current, regardless of any concurrent migration of the parent. The `cgroup_can_fork()` / `cgroup_post_fork()` bracketing prevents the cgroup set from being destroyed during this window.

The `sched_post_fork()` signature is also updated to accept `struct kernel_clone_args *kargs` so it can access the pinned `cset`. The call site in `kernel/fork.c` is updated accordingly: `sched_post_fork(p, args)`. This call occurs after `write_unlock_irq(&tasklist_lock)` but before `cgroup_post_fork()`, which is safe because `cgroup_can_fork()` has already locked the cgroup membership.

Note: This fix itself later introduced a different bug (commit `b1e8206582f9` fixes it) because moving `__set_task_cpu()` and `task_fork()` to after the task is visible in the pidhash created a window where an incompletely initialized task could be found by PID-based lookups. That subsequent fix introduced `sched_cgroup_fork()` to run between `cgroup_can_fork()` and `attach_pid()`.

## Triggering Conditions

- **Kernel version**: Any kernel from v3.6-rc1 (commit `8323f26ce342`) through v5.15 (one commit before `4ef0c5c6b5ba`). The bug exists in all kernels where `sched_fork()` accesses `sched_task_group` before `cgroup_can_fork()` pins the cgroup membership.
- **Configuration**: Requires `CONFIG_CGROUP_SCHED=y` (enabled by default on most distributions). The `sched_task_group` field only exists and is used under this config option.
- **CPUs**: At least 2 CPUs are required. One CPU runs the forking process, another runs the cgroup migration. The race is more likely to trigger with more CPUs.
- **Workload**: The workload must combine:
  1. A process that forks frequently (the `ebizzy` benchmark was the original trigger; any fork-heavy workload such as a web server, shell script with many subprocesses, or build system qualifies).
  2. Concurrent cgroup migration of the forking process — another process or management daemon moves the parent task between cgroups. This can be triggered by container orchestration, systemd scope management, or explicit `echo PID > /sys/fs/cgroup/.../cgroup.procs`.
  3. Cgroup destruction — the old cgroup must be removed (rmdir'd) after the parent is migrated out, so its `task_group` memory is freed. Without destruction, the pointer is stale but still dereferenceable (no crash, just wrong values).
- **Race window**: The window is between `dup_task_struct()` and `sched_fork()` within `copy_process()`. This is a relatively narrow window (a few microseconds), but under high fork rates (thousands of forks/second) combined with concurrent cgroup migration, the race is statistically likely to occur within minutes.
- **Probability**: Moderate. The original report came from a production system running `ebizzy` (a fork-intensive benchmark) with concurrent cgroup operations. It is not trivially reproducible with a single fork+migrate, but becomes likely under sustained concurrent stress.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP, though it requires careful concurrency setup and a minor kSTEP framework extension (a `kstep_cgroup_destroy()` function) for triggering the full use-after-free crash. Without cgroup destruction, the stale `sched_task_group` can still be detected via incorrect scheduling parameters.

### Setup

1. **QEMU configuration**: Use at least 2 CPUs (`-smp 2`). CPU 0 is reserved for the driver; the forking task runs on CPU 1. More CPUs increase the probability of hitting the race.
2. **Cgroup configuration**:
   - Create cgroup "A" with `kstep_cgroup_create("A")` and set a distinctive weight, e.g., `kstep_cgroup_set_weight("A", 100)`.
   - Create cgroup "B" with `kstep_cgroup_create("B")` and a different weight, e.g., `kstep_cgroup_set_weight("B", 10000)`.
3. **Task creation**:
   - Create a userspace task `p` with `kstep_task_create()`.
   - Pin `p` to CPU 1 with `kstep_task_pin(p, 1, 1)`.
   - Add `p` to cgroup A with `kstep_cgroup_add_task("A", p->pid)`.

### Triggering the Race

The race requires concurrent fork and cgroup migration. kSTEP's `kstep_task_fork()` sends a `SIGCODE_FORK` signal to the userspace task, which calls `fork()` in its signal handler — this traverses the real `copy_process()` → `dup_task_struct()` → `sched_fork()` path. The driver can immediately follow the signal with a cgroup migration to create concurrency.

The approach is to use a tight loop:
```
for (int i = 0; i < 1000; i++) {
    // Step 1: Ensure parent is in cgroup A
    kstep_cgroup_add_task("A", p->pid);
    kstep_sleep();  // Let migration complete

    // Step 2: Send fork signal (task will call fork() on CPU 1)
    // Use send_sig_info directly via KSYM_IMPORT to avoid the built-in sleep
    struct kernel_siginfo info = { .si_signo = SIGUSR1, .si_code = SIGCODE_FORK, .si_int = 1 };
    KSYM_IMPORT(send_sig_info);
    KSYM_send_sig_info(SIGUSR1, &info, p);

    // Step 3: Immediately migrate parent to cgroup B (racing with fork)
    kstep_cgroup_add_task("B", p->pid);

    // Step 4: (With extension) Destroy cgroup A to free task_group
    // kstep_cgroup_destroy("A");  // Triggers use-after-free if race is hit

    // Step 5: Sleep to let operations complete
    kstep_sleep();

    // Step 6: Recreate cgroup A for next iteration
    // kstep_cgroup_create("A");
    // kstep_cgroup_set_weight("A", 100);
}
```

The key idea is that `send_sig_info` delivers the signal to task `p` on CPU 1, which starts `fork()` → `copy_process()`. Meanwhile, the driver thread on CPU 0 immediately calls `kstep_cgroup_add_task("B", p->pid)`, which writes to cgroup B's `cgroup.procs` file, triggering `cgroup_migrate()`. If the cgroup migration completes between `dup_task_struct()` and `sched_fork()` in the fork path on CPU 1, the race is hit.

### Detection

**Method 1 — Crash detection (requires kstep_cgroup_destroy extension):**
If cgroup A is destroyed after the parent migrates out, the child's `sched_task_group` pointer becomes dangling. When `sched_fork()` accesses it, the kernel will oops with a NULL pointer dereference in `sched_slice()`. The driver can detect this via the absence of fork completion (the task never returns from fork) or by checking for kernel oops messages in `dmesg`. On the buggy kernel, this would crash; on the fixed kernel, `sched_post_fork()` re-derives `sched_task_group` from `kargs->cset` so no stale pointer is accessed.

**Method 2 — Stale task group detection (no extension needed):**
Without cgroup destruction, the old cgroup's `task_group` still exists but is the wrong one. After each fork, examine the child task's `sched_task_group`:
- Access `child->sched_task_group` via `KSYM_IMPORT` or through internal.h's `task_group(child)`.
- Compare it to cgroup B's task group (which is where the parent currently resides).
- On the buggy kernel, if the race is hit, the child may temporarily have cgroup A's `task_group` (the stale one from `dup_task_struct`) during the `sched_fork()` window. However, `cgroup_post_fork()` will correct the child's cgroup placement afterward, so the detection window is narrow.
- A more robust detection: compare the child's initial `se.vruntime` against cgroup A's and cgroup B's `cfs_rq->min_vruntime`. If the child's vruntime was computed using A's `cfs_rq` when the parent is in B, the race was hit.

**Method 3 — Behavioral detection via weight difference:**
Set cgroup A weight to 1 and cgroup B weight to 10000 (or vice versa). The `sched_slice()` computation depends on `cfs_rq->nr_running` and the task's relative weight within the group. If the child's computed time slice corresponds to the wrong group's parameters, the bug was triggered. Log the child's `se.slice` (or equivalent) immediately after fork via an `on_tick_begin` callback and compare against expected values.

### Required kSTEP Extensions

1. **`kstep_cgroup_destroy(name)`**: A function to rmdir a cgroup directory, analogous to `kstep_cgroup_create()` but calling `vfs_rmdir()` instead of `vfs_mkdir()`. This would allow triggering the full use-after-free crash by destroying the old cgroup after migrating the parent out. Implementation: add a `kstep_cgroup_rmdir()` function in `kernel.c` that calls `kern_path()` + `vfs_rmdir()` on the cgroup path.
2. **Direct signal sending**: The driver should be able to send `SIGCODE_FORK` without the automatic `kstep_sleep()` that `kstep_task_signal()` includes. This can be worked around by using `KSYM_IMPORT(send_sig_info)` directly in the driver, or by adding a `kstep_task_signal_nowait()` variant.

### Expected Behavior

- **Buggy kernel (before fix)**: The child task's `sched_task_group` is copied from the parent during `dup_task_struct()` and never updated before `sched_fork()` accesses it. If the parent is migrated to a different cgroup between `dup_task_struct()` and `sched_fork()`, the child accesses a stale (and potentially freed) `sched_task_group`. With cgroup destruction, this causes a kernel panic (NULL pointer dereference in `sched_slice()`). Without destruction, this causes incorrect `vruntime` initialization.
- **Fixed kernel (after fix)**: The `__set_task_cpu()` and `task_fork_fair()` calls are moved to `sched_post_fork()`, which runs after `cgroup_can_fork()` has pinned the cgroup membership. The child's `sched_task_group` is explicitly set from `kargs->cset->subsys[cpu_cgrp_id]`, which is always valid and current. No stale pointer access occurs regardless of concurrent parent migration. The child always gets correct scheduling parameters.
