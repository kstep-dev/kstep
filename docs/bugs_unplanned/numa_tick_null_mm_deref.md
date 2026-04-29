# NUMA: NULL mm Dereference in task_tick_numa After PF_KTHREAD Rework

**Commit:** `b3f9916d81e8ffb21cbe7abccf63f86a5a1d598a`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.19-rc1
**Buggy since:** v5.19-rc1 (introduced by `1b2552cbdbe0` "fork: Stop allowing kthreads to call execve", same merge window)

## Bug Description

The Linux kernel's NUMA balancing subsystem periodically drives memory fault scanning through `task_tick_numa()`, which is called on every scheduler tick for CFS tasks. To avoid scanning tasks that have no address space (such as kernel threads), `task_tick_numa()` historically relied on the `PF_KTHREAD` process flag to filter out such tasks. Specifically, the guard was:

```c
if ((curr->flags & (PF_EXITING | PF_KTHREAD)) || work->next != work)
    return;
```

A series of commits by Eric W. Biederman reworked how the init process (PID 1) and user mode helper (UMH) processes are created. Previously, these processes were created via `kernel_thread()` and inherited `PF_KTHREAD` from their parent until they called `kernel_execve()`, at which point `begin_new_exec()` cleared `PF_KTHREAD`. The rework introduced `user_mode_thread()` (commit `1b2552cbdbe0`), which creates threads without setting `PF_KTHREAD` at all, and commit `753550eb0ce1` made `PF_KTHREAD` setting explicit in `copy_process()` based on the `args->kthread` flag rather than inheriting it from the parent.

After these changes, the init process and UMH processes no longer have `PF_KTHREAD` set at any point in their lifecycle—even during the early phase before they call `kernel_execve()` when they have not yet acquired an `mm_struct` (i.e., `current->mm == NULL`). This means that during the window between process creation and the `execve` call, `task_tick_numa()` no longer filters out these tasks, and proceeds to dereference `curr->mm` in downstream functions, causing a NULL pointer dereference.

## Root Cause

The root cause lies in the assumption that `PF_KTHREAD` is a reliable proxy for "this task has no mm_struct." Before the fork rework, this assumption held: all tasks without an address space were kernel threads with `PF_KTHREAD` set. The rework broke this invariant by introducing a new category of tasks—user mode bootstrap tasks (init and UMH)—that lack `PF_KTHREAD` but also temporarily lack an `mm`.

The crash path is as follows:

1. `task_tick_fair()` calls `task_tick_numa(rq, curr)` if `sched_numa_balancing` is enabled.
2. `task_tick_numa()` checks `(curr->flags & (PF_EXITING | PF_KTHREAD))`. For the init process post-rework, neither flag is set, so the function does NOT return early.
3. It also checks `work->next != work`. The `numa_work` callback head is initialized to self-reference (`p->numa_work.next = &p->numa_work`) in `init_numa_balancing()`, so this check also does not trigger early return.
4. The function computes `period = (u64)curr->numa_scan_period * NSEC_PER_MSEC`. With `numa_scan_period` set to `sysctl_numa_balancing_scan_delay` (default 1000ms), `period` is 1 second in nanoseconds.
5. When the task's `sum_exec_runtime` exceeds `node_stamp + period` (i.e., after ~1 second of CPU time), and `node_stamp == 0` (initial state), it calls `task_scan_start(curr)`.
6. `task_scan_start()` calls `task_scan_min()`, which calls `task_nr_scan_windows(p)`.
7. `task_nr_scan_windows()` executes `rss = get_mm_rss(p->mm)`, which dereferences `p->mm`. Since `p->mm` is `NULL`, this results in a NULL pointer dereference at offset `0x3d0` (the offset of an `mm_struct` counter field from address 0).

The critical invariant that was broken: tasks created by `user_mode_thread()` have `->mm == NULL` and `PF_KTHREAD` cleared simultaneously, a state that previously did not exist in the kernel.

## Consequence

The bug causes a boot crash manifesting as a KASAN null-ptr-deref (or a plain NULL pointer dereference on non-KASAN kernels):

```
BUG: KASAN: null-ptr-deref in task_nr_scan_windows.isra.0
 arch_atomic_long_read at ./include/linux/atomic/atomic-long.h:29
 (inlined by) atomic_long_read at ./include/linux/atomic/atomic-instrumented.h:1266
 (inlined by) get_mm_counter at ./include/linux/mm.h:1996
 (inlined by) get_mm_rss at ./include/linux/mm.h:2049
 (inlined by) task_nr_scan_windows at kernel/sched/fair.c:1123
 Read of size 8 at addr 00000000000003d0 by task swapper/0/1
```

This is a fatal kernel crash during early boot, making the system completely unbootable. The crash occurs in the init process (PID 1, shown as `swapper/0/1`) which is the first user-space process. Since PID 1 must successfully boot the system, this crash prevents the kernel from completing initialization. There is no workaround short of disabling NUMA balancing at boot time (`numa_balancing=disable`) or reverting the fork rework commits.

The crash is deterministic on NUMA-capable systems with `CONFIG_NUMA_BALANCING=y` enabled (which is the default on most distribution kernels for multi-socket systems). The timing depends on how quickly the init process accumulates 1 second of CPU time, but on modern hardware this happens within the first few seconds of boot.

## Fix Summary

The fix adds an explicit `!curr->mm` check at the beginning of `task_tick_numa()`, before the existing `PF_KTHREAD` and `PF_EXITING` checks:

```c
- if ((curr->flags & (PF_EXITING | PF_KTHREAD)) || work->next != work)
+ if (!curr->mm || (curr->flags & (PF_EXITING | PF_KTHREAD)) || work->next != work)
    return;
```

This directly tests the condition that matters—whether the task has an address space—rather than relying on the `PF_KTHREAD` flag as an indirect proxy. The `PF_KTHREAD` check is retained for defense-in-depth (kernel threads should still be filtered even if they somehow acquired an mm), but `!curr->mm` is now the primary guard against accessing a NULL mm pointer.

This fix is correct and complete because it addresses the root cause: the function's guard condition was checking the wrong thing. The `!curr->mm` check is a direct, accurate test for the condition the function needs to avoid. It handles all possible future scenarios where a task might lack an mm (not just the init/UMH case), making the code more robust against further changes to process creation semantics. The fix is also consistent with how other functions in the same file (e.g., `update_scan_period()`) already check `!p->mm` directly.

## Triggering Conditions

The following precise conditions must all hold simultaneously to trigger the bug:

1. **Kernel version**: The kernel must contain commits `753550eb0ce1` ("fork: Explicitly set PF_KTHREAD") and `1b2552cbdbe0` ("fork: Stop allowing kthreads to call execve") but NOT the fix commit `b3f9916d81e8`. In practice, this was a narrow window during the v5.19 merge window.

2. **CONFIG_NUMA_BALANCING=y**: The kernel must be compiled with NUMA balancing support. This is the default for most distribution kernel configs on x86_64.

3. **NUMA balancing enabled at runtime**: The `sched_numa_balancing` static key must be enabled. On NUMA-capable systems, this is enabled by default. On non-NUMA systems, it is typically disabled, preventing the crash.

4. **Init process or UMH process must run**: The init process (PID 1) or a user mode helper process must be running and accumulating CPU time. The init process is guaranteed to run during boot, making this a boot-time crash.

5. **Sufficient runtime accumulation**: The task must accumulate more than `sysctl_numa_balancing_scan_delay` milliseconds (default: 1000ms) of CPU runtime (`sum_exec_runtime > node_stamp + period`). Since `node_stamp` starts at 0 for the init process, this triggers after ~1 second of init's CPU time.

6. **Task in pre-execve state**: The crash occurs in the window between the task's creation via `user_mode_thread()` and its call to `kernel_execve()`. During this window, `->mm` is NULL and `PF_KTHREAD` is not set. For the init process, this window covers the entire `kernel_init()` function up until `kernel_execve()` is called.

The bug is highly deterministic on affected kernels with NUMA balancing enabled—it triggers every single boot. There is no race condition or timing sensitivity beyond the runtime accumulation threshold.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reliably reproduced with kSTEP for the following reasons:

### 1. Why kSTEP Cannot Reproduce This Bug

The core trigger requires a task in a very specific state: `PF_KTHREAD` is NOT set AND `->mm` is NULL. This state only arises for processes created with `user_mode_thread()` (introduced by the same patch series that caused the bug) before they call `kernel_execve()`. kSTEP has no mechanism to create a task in this state through its public API:

- **kstep_task_create()** and **kstep_kthread_create()** both create kernel threads, which have `PF_KTHREAD` set and `->mm == NULL`. These tasks are correctly filtered by `task_tick_numa()`'s `PF_KTHREAD` check, so the bug is NOT triggered.
- **kstep_task_fork()** forks a process that inherits the parent's mm, so the resulting task has a valid `->mm` and does not trigger the NULL deref path at all.
- There is no `kstep_user_mode_thread()` or equivalent API that creates a task without `PF_KTHREAD` and without an `mm`.

### 2. What Would Need to Be Added to kSTEP

To reproduce this bug, kSTEP would need one of:

- **Direct flag manipulation**: Clear `PF_KTHREAD` on a kthread created by `kstep_kthread_create()`. While kSTEP provides access to `task_struct` internals via `internal.h`, directly clearing `PF_KTHREAD` on a kthread is dangerous—it fundamentally changes the task's identity and may cause cascading issues in other kernel subsystems that check `PF_KTHREAD` (e.g., signal handling, `kthread_stop()`, `to_kthread()` lookups). This is manipulating fundamental process identity, not scheduler state, and could lead to unrelated crashes that obscure the bug being tested.

- **A new `kstep_user_mode_thread()` API**: This would wrap the kernel's `user_mode_thread()` function to create a task that matches the exact state of the init process pre-execve. However, `user_mode_thread()` is specifically designed for creating processes that will call `kernel_execve()`, and the resulting task requires careful lifecycle management (it expects to exec into a userspace binary). kSTEP has no mechanism to provide a userspace binary or manage the execve lifecycle.

- **A `kstep_task_clear_kthread()` helper**: A safer API that clears `PF_KTHREAD` while handling the associated bookkeeping (freeing kthread_struct, etc.). This is a significant addition that would need careful design to avoid breaking kernel invariants.

### 3. Additional Complications

Even with the flag manipulation approach, there are further obstacles:

- **CONFIG_NUMA_BALANCING**: The kSTEP QEMU kernel must be built with `CONFIG_NUMA_BALANCING=y` and NUMA topology must be configured. While kSTEP has `kstep_topo_set_node()` for NUMA topology, the static key `sched_numa_balancing` must be enabled, which typically requires the kernel to detect NUMA hardware during boot.

- **Runtime accumulation**: The task needs to accumulate ~1 second of CPU runtime before `task_tick_numa` triggers the crash path. This requires approximately 250 scheduler ticks at HZ=250, which is feasible with `kstep_tick_repeat()` but adds significant execution time.

- **Boot-time nature**: The real bug manifests during early boot before any modules are loaded. A kernel module-based reproducer runs post-boot, by which time the init process has already passed through the vulnerable window (or crashed). The bug window is inherently a pre-module-load condition.

### 4. Alternative Reproduction Methods

Outside of kSTEP, this bug can be reproduced by:

1. **Building a kernel with the buggy commits applied**: Check out the kernel at commit `b3f9916d81e8~1`, ensure `CONFIG_NUMA_BALANCING=y` is set, and boot on a NUMA-capable system (or QEMU with `-numa node` options). The crash will occur deterministically during boot.

2. **QEMU with NUMA topology**: Run `qemu-system-x86_64 -smp 4 -numa node,cpus=0-1 -numa node,cpus=2-3` with the buggy kernel. The crash should appear in the kernel log within seconds of boot.

3. **Reverting the fix**: On a kernel that contains both the fork rework and the fix, reverting just `b3f9916d81e8` and booting will reproduce the crash.

### 5. Summary

This bug is fundamentally a boot-time crash caused by a change in process creation semantics that is outside kSTEP's ability to replicate. The `user_mode_thread()` function and the pre-execve task state it produces cannot be modeled by kSTEP's task creation APIs, and the workaround of directly clearing `PF_KTHREAD` on a kthread is too invasive and fragile to be a reliable reproduction method. The bug is best reproduced by simply booting the affected kernel version on NUMA hardware.
