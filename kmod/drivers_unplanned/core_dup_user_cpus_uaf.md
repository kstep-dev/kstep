# Core: Use-after-free in dup_user_cpus_ptr() during fork/setaffinity race

**Commit:** `87ca4f9efbd7cc649ff43b87970888f2812945b8`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.2-rc4
**Buggy since:** v5.15-rc1 (commit `07ec77a1d4e8` for arm64), v6.2-rc1 (commit `851a723e45d1` for all architectures)

## Bug Description

The `dup_user_cpus_ptr()` function in `kernel/sched/core.c` is called during the `fork()`/`clone()` path (specifically from `sched_fork()` → `dup_user_cpus_ptr()`) to duplicate the parent task's `user_cpus_ptr` into the newly created child task. The `user_cpus_ptr` field stores the user-requested CPU affinity mask, which is set when a process calls `sched_setaffinity()`. This pointer can also be cleared and freed by `do_set_cpus_allowed()`, which is invoked by `kthread_bind()` and `select_fallback_rq()`.

The bug is a use-after-free (UAF) and potentially a double-free that occurs when `dup_user_cpus_ptr()` races with a concurrent `do_set_cpus_allowed()` call on the same source task. Since `sched_setaffinity()` can be invoked by another process targeting an arbitrary PID, the target process may simultaneously be in the middle of a `fork()` call. The old `dup_user_cpus_ptr()` code checks `src->user_cpus_ptr` without holding any lock, allocates memory for `dst->user_cpus_ptr`, and then copies the content of `src->user_cpus_ptr` under `pi_lock`. However, by the time the copy happens, `src->user_cpus_ptr` may have already been freed by a concurrent `do_set_cpus_allowed()`, leading to a dereference of freed memory.

This bug was originally introduced in commit `07ec77a1d4e8` ("sched: Allow task CPU affinity to be restricted on asymmetric systems") which added the `user_cpus_ptr` mechanism for arm64 asymmetric CPU systems. It was temporarily fixed by commit `8f9ea86fdf99` ("sched: Always preserve the user requested cpumask") which ensured that `user_cpus_ptr`, once set, was never cleared during a task's lifetime. However, the bug was re-introduced in commit `851a723e45d1` ("sched: Always clear user_cpus_ptr in do_set_cpus_allowed()") which re-enabled the clearing of `user_cpus_ptr` in `do_set_cpus_allowed()` for correct semantics in `kthread_bind()` and `select_fallback_rq()` scenarios. After commit `851a723e45d1`, the bug affects all architectures, not just arm64.

## Root Cause

The root cause is a missing lock protection in `dup_user_cpus_ptr()` when reading the source task's `user_cpus_ptr`. The buggy code path is:

```c
int dup_user_cpus_ptr(struct task_struct *dst, struct task_struct *src, int node)
{
    unsigned long flags;

    if (!src->user_cpus_ptr)       // [1] Check without lock — RACY
        return 0;

    dst->user_cpus_ptr = kmalloc_node(cpumask_size(), GFP_KERNEL, node);  // [2] Allocate for dst
    if (!dst->user_cpus_ptr)
        return -ENOMEM;

    raw_spin_lock_irqsave(&src->pi_lock, flags);
    cpumask_copy(dst->user_cpus_ptr, src->user_cpus_ptr);  // [3] Copy — src may be freed!
    raw_spin_unlock_irqrestore(&src->pi_lock, flags);
    return 0;
}
```

Concurrently, `do_set_cpus_allowed()` can clear and free `src->user_cpus_ptr`:

```c
void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
    struct affinity_context ac = {
        .new_mask  = new_mask,
        .user_mask = NULL,
        .flags     = SCA_USER,   // This flag causes user_cpus_ptr to be swapped out
    };
    __do_set_cpus_allowed(p, &ac);
    kfree(ac.user_mask);           // [4] Frees the old user_cpus_ptr
}
```

Inside `__do_set_cpus_allowed()` → `set_cpus_allowed_common()`, when `SCA_USER` is set:
```c
if (ctx->flags & SCA_USER)
    swap(p->user_cpus_ptr, ctx->user_mask);  // [5] Swaps user_cpus_ptr to NULL
```

The race occurs as follows:
1. **Thread A** (forking process): At step [1], `dup_user_cpus_ptr()` observes `src->user_cpus_ptr` as non-NULL. It proceeds to allocate memory at step [2].
2. **Thread B** (concurrent `do_set_cpus_allowed()`): Between steps [1] and [3] (or even during [3]), step [5] swaps `src->user_cpus_ptr` to NULL and captures the old pointer in `ctx->user_mask`. Then step [4] frees it via `kfree()`.
3. **Thread A**: At step [3], `cpumask_copy()` dereferences `src->user_cpus_ptr`, which now points to freed memory (use-after-free), or is NULL (NULL dereference if the swap happened before the lock was acquired but the check at [1] already passed).

There is also a potential double-free scenario: if Thread A at step [1] sees `src->user_cpus_ptr` as non-NULL, allocates memory and assigns it to `dst->user_cpus_ptr`, but then at step [3] under the lock, `src->user_cpus_ptr` has become NULL (from the concurrent swap), the `cpumask_copy()` would read from NULL/freed memory. Additionally, in the original arm64-specific code path, the clearing happened via `__set_cpus_allowed_ptr_locked()` which holds `pi_lock`, creating a window where the pointer is freed but the locked copy in `dup_user_cpus_ptr()` still uses the stale value it read before locking.

A second subtle issue is that the old code assigns `dst->user_cpus_ptr` in step [2] before the locked check. If `src->user_cpus_ptr` becomes NULL by the time the lock is acquired, the child task ends up with an allocated `user_cpus_ptr` that contains uninitialized (or garbage) data from `src->user_cpus_ptr` (which has been freed), since the `cpumask_copy` would read freed memory. This means the child's affinity mask could be corrupted.

## Consequence

The immediate consequence is a **use-after-free** vulnerability in the kernel scheduler. When `dup_user_cpus_ptr()` dereferences the freed `src->user_cpus_ptr` in the `cpumask_copy()` call, it reads from memory that has been returned to the SLAB allocator. Depending on timing and memory allocator behavior:

1. **Kernel crash (NULL pointer dereference or page fault)**: If the freed memory page has been unmapped or the pointer has been poisoned by KASAN/debug allocators, the kernel will oops with a page fault in the `cpumask_copy()` function, crashing the forking process and potentially the entire system.
2. **Silent data corruption**: If the freed memory has been reallocated for a different purpose, `cpumask_copy()` reads arbitrary data into the child's `user_cpus_ptr`. This corrupts the child's CPU affinity mask, potentially restricting or expanding which CPUs the child can run on in unpredictable ways.
3. **Double-free**: In some race orderings, the same `cpumask` allocation could be freed twice — once by `do_set_cpus_allowed()` and once by the child's eventual `release_user_cpus_ptr()` — corrupting the SLAB allocator's freelist and potentially leading to exploitable heap corruption.
4. **Security implications**: As a use-after-free in the kernel, this bug is potentially exploitable for privilege escalation. An attacker who can trigger concurrent `fork()` and `sched_setaffinity()` calls on the same process could potentially influence what data the child's `user_cpus_ptr` points to.

The bug was reported by David Wang (王标) from Xiaomi, likely encountered in production on arm64 devices where asymmetric CPU affinity is actively used (big.LITTLE systems). With the re-introduction via commit `851a723e45d1`, the bug became reachable on all architectures, not just arm64.

## Fix Summary

The fix restructures `dup_user_cpus_ptr()` to eliminate the race window in three key ways:

1. **Pre-clear `dst->user_cpus_ptr`**: The function now explicitly sets `dst->user_cpus_ptr = NULL` at the very beginning. This ensures that if the function returns early (because `src->user_cpus_ptr` became NULL), the child does not inherit a stale or uninitialized pointer. In the old code, `dst->user_cpus_ptr` was set to the kmalloc result before verifying under lock that `src->user_cpus_ptr` was still valid.

2. **Racy pre-check with `data_race()`**: The initial check `if (data_race(!src->user_cpus_ptr)) return 0;` is deliberately marked as a data race using the `data_race()` annotation. This is an intentional optimization — if `user_cpus_ptr` is NULL (the common case for most tasks), the function returns immediately without acquiring `pi_lock`, avoiding unnecessary lock overhead on every fork. The race (another thread setting `user_cpus_ptr` between this check and the locked check) is benign because the locked check handles it.

3. **Deferred pointer assignment under lock**: The allocated `user_mask` buffer is stored in a local variable rather than immediately assigned to `dst->user_cpus_ptr`. Inside the `pi_lock` critical section, the function re-checks `src->user_cpus_ptr`. If it is still valid, it uses `swap(dst->user_cpus_ptr, user_mask)` to atomically install the allocated buffer as the child's `user_cpus_ptr` and then copies the source mask. If `src->user_cpus_ptr` has been cleared by a concurrent `do_set_cpus_allowed()`, the function simply frees the allocated buffer and returns with `dst->user_cpus_ptr` still NULL. This ensures that the dereference of `src->user_cpus_ptr` in `cpumask_copy()` only occurs when it is known to be valid (protected by `pi_lock`).

The fixed code:
```c
raw_spin_lock_irqsave(&src->pi_lock, flags);
if (src->user_cpus_ptr) {
    swap(dst->user_cpus_ptr, user_mask);
    cpumask_copy(dst->user_cpus_ptr, src->user_cpus_ptr);
}
raw_spin_unlock_irqrestore(&src->pi_lock, flags);
if (unlikely(user_mask))
    kfree(user_mask);
```

This is correct because `do_set_cpus_allowed()` clears `user_cpus_ptr` via `swap(p->user_cpus_ptr, ctx->user_mask)` inside `set_cpus_allowed_common()`, which is called under `pi_lock` (from `__set_cpus_allowed_ptr_locked()`). Therefore, the re-check of `src->user_cpus_ptr` under `pi_lock` is properly synchronized with the clearing path.

## Triggering Conditions

The bug requires the following precise conditions to manifest:

1. **A task with `user_cpus_ptr` set**: The target task must have previously called (or had called on its behalf) `sched_setaffinity()`, which allocates and populates `user_cpus_ptr`. This is common on arm64 big.LITTLE systems where the kernel automatically restricts task affinity, and on any system where a process or container manager sets CPU affinity.

2. **Concurrent `fork()` and `do_set_cpus_allowed()`**: The task must be calling `fork()` (or `clone()`) while simultaneously another code path calls `do_set_cpus_allowed()` on the same task. `do_set_cpus_allowed()` is called from two places:
   - `kthread_bind()`: binds a kernel thread to specific CPUs (not applicable to userspace tasks)
   - `select_fallback_rq()`: reassigns a task when its current CPU goes offline (CPU hotplug)

   For userspace tasks, the relevant trigger is CPU hotplug: when a CPU goes offline, `select_fallback_rq()` is called for any task affined to that CPU, which invokes `do_set_cpus_allowed()` to clear `user_cpus_ptr` and reset the task's affinity.

3. **Precise timing window**: The race window is between the unlocked check of `src->user_cpus_ptr` (or the locked `cpumask_copy`) and the concurrent `swap()` + `kfree()` in `do_set_cpus_allowed()`. This window is on the order of nanoseconds to low microseconds, depending on the time spent in `kmalloc_node()` between the check and the lock acquisition.

4. **Multi-CPU system**: The race requires true concurrent execution on at least two CPUs — one CPU executing the `fork()` path and another executing `do_set_cpus_allowed()`.

5. **Kernel version**: The bug exists in kernels from v5.15-rc1 (arm64 only, via `07ec77a1d4e8`) and from v6.2-rc1 (all architectures, via `851a723e45d1`). However, between commits `8f9ea86fdf99` and `851a723e45d1`, the bug was temporarily fixed because `user_cpus_ptr` was never cleared once set.

The probability of triggering this race in normal operation is low because it requires the precise overlap of `fork()` with a CPU hotplug event (for userspace tasks) or `kthread_bind()` (for kernel threads). However, on systems with frequent CPU hotplug events (e.g., dynamic power management on mobile devices, cloud VMs with vCPU hotplug), the probability increases significantly, especially under heavy fork workloads.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why This Bug Cannot Be Reproduced

**The bug is a memory safety issue, not a scheduling logic/behavior bug.** kSTEP is designed to observe and verify scheduler state and decisions (task placement, load balancing, EEVDF ordering, etc.). This bug is a use-after-free in a cpumask pointer during the fork path — it corrupts memory rather than producing incorrect scheduling decisions. Even if the race were triggered, kSTEP's observation facilities (`kstep_eligible()`, `kstep_output_curr_task()`, `kstep_output_balance()`, etc.) cannot detect freed-memory dereferences or heap corruption. Detection requires KASAN (Kernel Address Sanitizer) or similar memory debugging infrastructure.

**kSTEP lacks `sched_setaffinity()` support to set `user_cpus_ptr`.** The `user_cpus_ptr` field is only populated by the `sched_setaffinity()` syscall path (specifically the `user_mask` allocation in `sched_setaffinity()` → `__set_cpus_allowed_ptr()`). kSTEP's `kstep_task_pin()` calls `set_cpus_allowed_ptr()`, which does NOT set `user_cpus_ptr` — it only modifies `p->cpus_mask`. Without `user_cpus_ptr` being set on the source task, `dup_user_cpus_ptr()` returns immediately at the first check (`if (!src->user_cpus_ptr) return 0;`), and the race cannot occur.

**kSTEP cannot orchestrate concurrent `fork()` and `do_set_cpus_allowed()` on the same task.** kSTEP's `kstep_task_fork()` sends a signal to a userspace task and then calls `kstep_sleep()`, blocking the driver until the fork completes. This means the driver executes sequentially — it cannot simultaneously trigger `do_set_cpus_allowed()` on the same task during the fork window. While kSTEP kthreads run concurrently, `do_set_cpus_allowed()` is not directly accessible through any kSTEP API — `kstep_kthread_bind()` calls `set_cpus_allowed_ptr()` (not `do_set_cpus_allowed()`), and it operates on kthreads, not userspace tasks.

**The race window is extremely small and requires uncontrolled timing.** The vulnerable window in `dup_user_cpus_ptr()` spans from the unlocked `if (!src->user_cpus_ptr)` check through `kmalloc_node()` to the `cpumask_copy()` inside the `pi_lock` critical section — a window of perhaps a few microseconds. Hitting this window requires true non-deterministic concurrent execution, which contradicts kSTEP's deterministic, controlled execution model.

### 2. What Would Need to Be Added to kSTEP

To support reproducing this bug, kSTEP would need several fundamental additions:

- **`kstep_task_setaffinity(p, mask)` API**: A new function that calls the kernel's `sched_setaffinity()` (or an equivalent internal path) to set `user_cpus_ptr` on a kSTEP-managed userspace task. This would require either importing `sched_setaffinity` via `KSYM_IMPORT` or directly allocating and setting `user_cpus_ptr` and calling `__set_cpus_allowed_ptr()` with appropriate flags.

- **Concurrent operation support**: A mechanism to run two operations on the same task simultaneously from the driver. Currently, kSTEP's signal-based task control (`kstep_task_fork` sends signal + sleeps) is inherently sequential. One approach would be to schedule a delayed `do_set_cpus_allowed()` call via a timer or workqueue that fires during the fork window, but this still relies on timing luck.

- **CPU hotplug simulation**: Since `do_set_cpus_allowed()` is primarily triggered for userspace tasks via `select_fallback_rq()` during CPU hotplug, a `kstep_cpu_hotplug(cpu, online)` API would be needed to simulate CPU offline events that trigger the clearing path.

- **Memory safety detection**: kSTEP would need KASAN integration or equivalent memory-checking capabilities to detect the use-after-free. Alternatively, the kernel build would need to enable `CONFIG_KASAN=y`.

### 3. Alternative Reproduction Methods

Outside kSTEP, this bug can be reproduced with:

1. **KASAN-enabled kernel + stress test**: Build a kernel with `CONFIG_KASAN=y`. Run a workload that forks rapidly while another thread repeatedly calls `sched_setaffinity()` on the forking process. On arm64 big.LITTLE systems, the asymmetric affinity restriction paths make this more likely to trigger.

2. **CPU hotplug stress test**: On a multi-CPU system, run a fork-heavy workload while rapidly onlining/offlining CPUs. The CPU offline path triggers `select_fallback_rq()` → `do_set_cpus_allowed()` which races with concurrent forks.

3. **syzkaller/syzbot**: Automated kernel fuzzers like syzkaller can exercise the `fork()`/`sched_setaffinity()` race by generating concurrent syscall sequences. KASAN will detect the UAF when triggered.

4. **Manual reproducer**: A C program with two threads — one calling `fork()` in a tight loop, and another calling `sched_setaffinity()` on the forking thread's PID — run on a KASAN-enabled kernel should eventually hit the race.
