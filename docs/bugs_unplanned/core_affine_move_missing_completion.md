# Core: Missing completion for affine_move_task() waiters in migration_cpu_stop()

**Commit:** `d707faa64d03d26b529cc4aea59dab1b016d4d33`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.11-rc1
**Buggy since:** v5.10-rc1 (introduced by commit `6d337eab041d` "sched: Fix migrate_disable() vs set_cpus_allowed_ptr()")

## Bug Description

When `sched_setaffinity()` is called on a task, the kernel's `__set_cpus_allowed_ptr()` function may need to use the stop-machine mechanism to migrate the task to a CPU within the new allowed mask. In the migrate_disable() rework introduced by commit `6d337eab041d`, a new `affine_move_task()` function was added that uses a `set_affinity_pending` structure with a completion to synchronize the affinity change. When the task is currently running (`task_running()` case), `affine_move_task()` schedules a stopper via `stop_one_cpu()` and then blocks on `wait_for_completion(&pending->done)`, expecting `migration_cpu_stop()` to eventually signal the completion.

The bug occurs when `migration_cpu_stop()` runs on the target CPU but finds that the task has already moved to a different runqueue (`task_rq(p) != rq`). In the original buggy code, the `else if` branch in `migration_cpu_stop()` only checked `dest_cpu < 0`, which corresponds to the `migrate_enable()` path. When the stopper was initiated by `set_cpus_allowed_ptr()` (where `dest_cpu >= 0`), and the task had moved between the time the stopper was scheduled and when it actually ran, the code would fall through to the `out:` label without ever signaling the completion. This left the `affine_move_task()` caller permanently stuck in `wait_for_completion()`.

This race is possible with both PREEMPT and !PREEMPT kernels. In PREEMPT kernels, the task can be preempted between the lock release and the stopper execution, allowing it to be migrated elsewhere. In !PREEMPT kernels, the window is even larger because the task can voluntarily block and be woken up on a different CPU before the stopper runs. The bug was discovered by Qian Cai using a syscall fuzzer (Trinity) that aggressively called `sched_setaffinity()`, producing a hung task warning after 368 seconds.

## Root Cause

The root cause lies in the control flow of `migration_cpu_stop()` as introduced by commit `6d337eab041d`. The function has two main branches based on whether the task is still on the expected runqueue:

1. **`if (task_rq(p) == rq)`**: The task is on the expected runqueue. The function handles the migration, clears `p->migration_pending`, and sets `complete = true` so that `complete_all(&pending->done)` is called after releasing locks.

2. **`else if (dest_cpu < 0)`**: The task has moved, and `dest_cpu < 0` indicates this was a `migrate_enable()` stopper. This branch handles the case where the task migrated between `migrate_enable()`'s `preempt_enable()` and the stopper execution. It either chases the task to its new CPU or confirms the task is already on a valid CPU.

The critical missing case is when `task_rq(p) != rq` **and** `dest_cpu >= 0`. This occurs when `set_cpus_allowed_ptr()` schedules a stopper via `stop_one_cpu()` from within `affine_move_task()`, but by the time the stopper runs, the task has been moved to a different runqueue. In the buggy code, neither branch matches this condition: the first branch fails because `task_rq(p) != rq`, and the second branch fails because `dest_cpu >= 0`. The code falls through directly to the `out:` label.

At the `out:` label, the function calls `task_rq_unlock()` and then checks the `complete` variable. Since `complete` was initialized to `false` and was never set to `true` (neither branch executed its completion logic), `complete_all(&pending->done)` is never called. Meanwhile, `affine_move_task()` is blocked indefinitely on `wait_for_completion(&pending->done)`.

The specific sequence that triggers this is:

1. Thread A calls `sched_setaffinity()` on Thread B, which calls `__set_cpus_allowed_ptr()` → `affine_move_task()`.
2. Thread B is currently running, so `affine_move_task()` takes the `task_running()` path: it schedules `stop_one_cpu(cpu_of(rq), migration_cpu_stop, &arg)` on the CPU where Thread B is running, and then calls `wait_for_completion(&pending->done)`.
3. Between the rq unlock in `affine_move_task()` and the stopper actually running on that CPU, Thread B gets preempted (or voluntarily sleeps and is woken) and ends up on a different CPU.
4. The stopper function `migration_cpu_stop()` runs. It locks Thread B's `pi_lock` and the local rq's lock. It finds `task_rq(p) != rq` because Thread B moved. It checks `dest_cpu < 0`, which is false (dest_cpu was set to a valid CPU by `affine_move_task()`). So neither branch executes.
5. The function jumps to `out:`, unlocks, and returns without calling `complete_all()`.
6. Thread A remains stuck forever in `wait_for_completion()`.

## Consequence

The observable impact is a permanent task hang. The calling thread (e.g., a process issuing `sched_setaffinity()`) becomes stuck in an uninterruptible sleep state (D state) waiting for a completion that will never be signaled. The hung task detector eventually fires, reporting the task has been blocked for an extended period (368 seconds in the reported case).

The stack trace from the bug report shows:
```
task:trinity-c30     state:D stack:26576 pid:91730
  __switch_to+0xf0/0x1a8
  __schedule+0x6ec/0x1708
  schedule+0x1bc/0x3b0
  schedule_timeout+0x3c4/0x4c0
  wait_for_completion+0x13c/0x248
  affine_move_task+0x410/0x688
  __set_cpus_allowed_ptr+0x1b4/0x370
  sched_setaffinity+0x4f0/0x7e8
  __arm64_sys_sched_setaffinity+0x1f4/0x2a0
```

This is a denial-of-service condition that can be triggered by an unprivileged user calling `sched_setaffinity()` on another task (or itself) under concurrent task migration. The `set_affinity_pending` structure is allocated on the stack of the stuck thread, so if the thread is somehow killed (which is difficult in D state), the pending structure becomes a dangling reference, potentially leading to use-after-free or further corruption. In practice, the thread simply hangs indefinitely, reducing system capacity. Under fuzzing or heavy affinity-setting workloads, multiple threads can become stuck simultaneously.

## Fix Summary

The fix changes the condition `else if (dest_cpu < 0)` to `else if (dest_cpu < 0 || pending)` in `migration_cpu_stop()`. This ensures that the rq-mismatch case is also entered when there is a pending affinity change (i.e., when `set_cpus_allowed_ptr()` initiated the stopper, not just `migrate_enable()`).

Within this expanded branch, the fix adds a new check at the top: if there is a `pending` and the task is already on a CPU within its allowed mask (`cpumask_test_cpu(task_cpu(p), p->cpus_ptr)`), then the migration is considered complete. The function sets `p->migration_pending = NULL`, sets `complete = true`, and jumps to `out:`, where `complete_all(&pending->done)` will be called. This is correct because the task successfully moved to an allowed CPU (even if it wasn't the specific `dest_cpu` that was chosen), and the purpose of `set_cpus_allowed_ptr()` is to ensure the task runs within the allowed mask, not necessarily on a specific CPU.

If the task has moved but is NOT on an allowed CPU (which shouldn't normally happen but is handled for robustness), the code continues with the existing logic for the `migrate_enable()` case: it either confirms the task is valid (if `!pending`), or chases the task to its new CPU by scheduling another stopper via `stop_one_cpu_nowait()`. This ensures that the completion is eventually signaled regardless of how many times the task moves between CPUs. The fix maintains the previous behavior for the `migrate_enable()` path (where `dest_cpu < 0`) while correctly handling the previously unhandled `set_cpus_allowed_ptr()` path (where `dest_cpu >= 0` and `pending != NULL`).

## Triggering Conditions

The following conditions must all be met to trigger this bug:

- **Kernel version**: Must be running a kernel with commit `6d337eab041d` applied but without the fix `d707faa64d03`. In practice, this means v5.10-rc1 through v5.10.x (the fix was merged into v5.11-rc1 and likely backported to stable).
- **SMP system**: At least 2 CPUs are required so that the task can actually migrate to a different CPU.
- **Concurrent affinity change and task migration**: One thread must call `sched_setaffinity()` (or any path through `set_cpus_allowed_ptr()`) on a target task, while the target task is running and can be migrated to a different CPU before the stopper executes.
- **Task must be running**: The target task must be in the `task_running()` state when `affine_move_task()` checks, so that the `stop_one_cpu()` path is taken (rather than the direct `move_queued_task()` path).
- **Task must move before stopper runs**: Between the rq unlock in `affine_move_task()` and the execution of `migration_cpu_stop()`, the target task must end up on a different runqueue. This can happen if the task is preempted and load-balanced to another CPU, or if it blocks and is woken up on a different CPU.
- **!PREEMPT makes it more likely**: On !PREEMPT kernels, the window between unlocking the rq and the stopper running is much larger, as the target task has a bigger opportunity to block voluntarily and be woken up elsewhere. However, the bug can also occur on PREEMPT kernels if the target is preempted and migrated.

The bug was discovered using the Trinity syscall fuzzer, which aggressively calls `sched_setaffinity()` with random parameters on random tasks. This creates the necessary concurrency pressure where affinity changes overlap with task migrations. A targeted reproducer would need to have one thread repeatedly calling `sched_setaffinity()` on a target while the target is actively running and subject to migration (e.g., by having it compete for CPU time with other tasks so that load balancing moves it around).

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reason:

1. **KERNEL VERSION TOO OLD**: The bug was introduced in commit `6d337eab041d` which is part of the v5.10-rc1 development cycle. The fix commit `d707faa64d03` was merged into v5.11-rc1. kSTEP supports Linux v5.15 and newer only. Since the buggy kernel (v5.10.x) is well below the v5.15 minimum, kSTEP cannot compile or run the required kernel version. By v5.15, this fix has long been included in the kernel, so the bug does not exist in any kernel version that kSTEP supports.

2. **What would be needed**: Even if the kernel version were supported, reproducing this bug would require the ability to trigger `sched_setaffinity()` from a real userspace process, since the bug is in the `__set_cpus_allowed_ptr()` → `affine_move_task()` path that is invoked by the `sched_setaffinity` syscall. kSTEP can call `kstep_task_pin()` to set CPU affinity from a kernel module, but the internal code path it takes may differ from the syscall path. Specifically, the race requires `affine_move_task()` to call `stop_one_cpu()` on a running task and then block on the completion, while the target task simultaneously migrates. kSTEP's `kstep_task_pin()` operates from kernel context and may not exercise the same `task_running()` → `stop_one_cpu()` → `wait_for_completion()` path that the syscall does.

3. **Race condition characteristics**: The bug is a race between the stopper being scheduled and the target task migrating to a different CPU. To reproduce it reliably, one would need to: (a) call `set_cpus_allowed_ptr()` on a running task so the `stop_one_cpu()` path is taken, (b) ensure the target task moves to another CPU before the stopper executes, and (c) have the stopper find `task_rq(p) != rq` with `dest_cpu >= 0`. This is a timing-sensitive race that was found with a fuzzer, suggesting it is non-trivial to trigger deterministically.

4. **Alternative reproduction methods**: Outside of kSTEP, this bug can be reproduced using the Trinity syscall fuzzer on a kernel between v5.10-rc1 and v5.10.x (before the fix was backported). The fuzzer should be configured to aggressively call `sched_setaffinity()` on random tasks with random CPU masks, on a multi-CPU system. A more targeted approach would be to have two threads: Thread A repeatedly calling `sched_setaffinity()` on Thread B, while Thread B continuously blocks and wakes up (e.g., using futex or sleep) to increase the likelihood of migrating between the rq unlock and stopper execution. Running on a !PREEMPT kernel increases the race window.

5. **Verification approach**: If one could run the buggy kernel, the bug manifests as the `sched_setaffinity()`-calling thread entering D state forever. The hung task detector (if enabled) reports it after `CONFIG_DEFAULT_HUNG_TASK_TIMEOUT` seconds. The stack trace shows `wait_for_completion` called from `affine_move_task` called from `__set_cpus_allowed_ptr`.
