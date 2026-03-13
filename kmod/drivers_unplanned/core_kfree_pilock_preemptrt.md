# Core: kfree() called with pi_lock held causes sleeping-in-atomic on PREEMPT_RT

**Commit:** `9a5418bc48babb313d2a62df29ebe21ce8c06c59`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.2-rc4
**Buggy since:** v6.2-rc1 (introduced by commit `851a723e45d1` "sched: Always clear user_cpus_ptr in do_set_cpus_allowed()")

## Bug Description

Commit `851a723e45d1` introduced a call to `kfree(ac.user_mask)` inside `do_set_cpus_allowed()` to free the `user_cpus_ptr` cpumask when the user-requested affinity mask is being cleared. This occurs when `do_set_cpus_allowed()` is called with `SCA_USER` flag and `user_mask = NULL`, causing `__do_set_cpus_allowed()` to swap out the task's `user_cpus_ptr` into `ac.user_mask`, which is then freed.

The problem is that several callers of `do_set_cpus_allowed()` invoke it while holding the task's `pi_lock` (a raw spinlock). On PREEMPT_RT kernels, `kfree()` can acquire sleeping locks internally (e.g., through the SLUB allocator's `__slab_free()` path which may call `local_bh_enable()` and trigger rescheduling). Calling a potentially-sleeping function while holding a raw spinlock is illegal on PREEMPT_RT, and lockdep also detects the circular lock dependency on non-RT kernels with `CONFIG_PROVE_LOCKING=y`.

There are three distinct call paths where `do_set_cpus_allowed()` is invoked under `pi_lock`:
1. `__kthread_bind_mask()` in `kernel/kthread.c` — acquires `p->pi_lock` via `raw_spin_lock_irqsave()` before calling `do_set_cpus_allowed()`.
2. `__balance_push_cpu_stop()` in `kernel/sched/core.c` — acquires `p->pi_lock` via `raw_spin_lock_irq()` before calling `select_fallback_rq()`, which may call `do_set_cpus_allowed()`.
3. `select_task_rq()` (called from `try_to_wake_up()`) — the caller holds `p->pi_lock` (asserted by `lockdep_assert_held(&p->pi_lock)` at line 3525), and if `is_cpu_allowed()` returns false, it calls `select_fallback_rq()` → `do_set_cpus_allowed()`.

For the `kfree()` to actually be called, the task must have a non-NULL `user_cpus_ptr`, which is set when `sched_setaffinity()` is called on the task. Any task that has ever had its CPU affinity explicitly set via the `sched_setaffinity()` syscall will have `user_cpus_ptr` allocated.

## Root Cause

The root cause is a violation of the PREEMPT_RT locking rules in `do_set_cpus_allowed()`. The function was modified by commit `851a723e45d1` to clear the user-requested cpumask by setting `SCA_USER` flag and `user_mask = NULL` in the `affinity_context`:

```c
void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
    struct affinity_context ac = {
        .new_mask  = new_mask,
        .user_mask = NULL,
        .flags     = SCA_USER,    /* clear the user requested mask */
    };

    __do_set_cpus_allowed(p, &ac);
    kfree(ac.user_mask);    // BUG: may sleep under pi_lock on PREEMPT_RT
}
```

Inside `__do_set_cpus_allowed()`, when `SCA_USER` is set and `ctx->user_mask` is NULL, the `set_cpus_allowed()` callback (specifically `set_cpus_allowed_common()`) swaps the task's `user_cpus_ptr` into `ctx->user_mask`. After `__do_set_cpus_allowed()` returns, `ac.user_mask` points to the old cpumask that was previously allocated by `sched_setaffinity()`, and `kfree(ac.user_mask)` is called to free it.

On PREEMPT_RT kernels, raw spinlocks (`raw_spin_lock`) are true spinning locks that disable preemption and cannot be held when calling any potentially-sleeping function. The `kfree()` function in PREEMPT_RT can sleep because the SLUB allocator converts its internal locks to sleeping locks (rt_mutex) under PREEMPT_RT. Specifically, the `__slab_free()` → `put_cpu_partial()` → `local_bh_enable()` path can trigger rescheduling.

The `pi_lock` is a `raw_spinlock_t`, meaning it remains a true spinning lock even on PREEMPT_RT (unlike regular `spinlock_t` which becomes a sleeping lock on PREEMPT_RT). Therefore, calling `kfree()` while holding `pi_lock` creates a situation where a sleeping function is called from a non-preemptible context. This triggers two distinct lockdep violations: (1) "WARNING: possible circular locking dependency detected" when lockdep detects that the slab allocator locks can create a cycle with `pi_lock`, and (2) "BUG: sleeping function called from invalid context" when the allocator actually attempts to sleep.

## Consequence

The observable consequences depend on kernel configuration:

On PREEMPT_RT kernels (`CONFIG_PREEMPT_RT=y`), the bug produces two types of kernel warnings. First, lockdep reports a circular locking dependency between `pi_lock` and the SLUB allocator's internal locks. Second, and more critically, a "BUG: sleeping function called from invalid context" splat is generated when `kfree()` attempts to acquire a sleeping lock while preemption is disabled by the raw spinlock. On PREEMPT_RT with `CONFIG_DEBUG_ATOMIC_SLEEP=y`, this results in a loud kernel BUG() that can halt the affected task. In the worst case on production PREEMPT_RT systems (e.g., `panic_on_warn=1`), this could cause a system panic.

On non-PREEMPT_RT kernels with `CONFIG_PROVE_LOCKING=y` (lockdep enabled), the circular lock dependency between `pi_lock` and slab allocator locks is still detected and reported as a lockdep warning. While the actual sleep does not occur on non-RT kernels (because `kfree()` uses spinning locks there), lockdep flags the potential deadlock. This warning is informational but causes noise in kernel logs and could mask other genuine lockdep warnings.

The kernel test robot also reported a general protection fault (null-ptr-deref at address `0x0000000000000000`) in `sched_setaffinity()` when testing the fix patch on a `!CONFIG_SMP` build, where `alloc_user_cpus_ptr()` returns NULL. This secondary issue was subsequently addressed by the check `if (IS_ENABLED(CONFIG_SMP) && !user_mask)` in the fix.

## Fix Summary

The fix replaces `kfree()` with `kfree_rcu()` in `do_set_cpus_allowed()` to defer the memory freeing to a RCU grace period, avoiding the need to call a potentially-sleeping function while holding `pi_lock`. A local `cpumask_rcuhead` union type is defined to overlay `struct rcu_head` on the freed cpumask memory:

```c
union cpumask_rcuhead {
    cpumask_t cpumask;
    struct rcu_head rcu;
};

__do_set_cpus_allowed(p, &ac);
kfree_rcu((union cpumask_rcuhead *)ac.user_mask, rcu);
```

`kfree_rcu()` is safe to call from any context (including raw spinlock context and with interrupts disabled) because it merely queues the memory to be freed later during an RCU callback, which runs in a context where sleeping is allowed.

To support this, a new `alloc_user_cpus_ptr()` helper function is introduced. This function ensures that the allocated buffer is large enough to hold both a `cpumask_t` and a `struct rcu_head` (since `kfree_rcu()` needs to store the `rcu_head` in the memory being freed). The allocation size is `max_t(int, cpumask_size(), sizeof(struct rcu_head))`. All allocation sites for `user_cpus_ptr` (`dup_user_cpus_ptr()` and `sched_setaffinity()`) are updated to use `alloc_user_cpus_ptr()`. For non-SMP configurations, `alloc_user_cpus_ptr()` returns NULL since `user_cpus_ptr` is not used, and `sched_setaffinity()` is modified with `IS_ENABLED(CONFIG_SMP)` to handle this case.

## Triggering Conditions

The following conditions must all be met to trigger the bug:

1. **PREEMPT_RT kernel**: The kernel must be compiled with `CONFIG_PREEMPT_RT=y` for the actual sleeping-in-atomic bug to manifest. On non-RT kernels, `CONFIG_PROVE_LOCKING=y` (lockdep) is sufficient to detect the circular dependency warning, but the actual sleep does not occur.

2. **Task with user_cpus_ptr set**: A task must have had its CPU affinity explicitly set via the `sched_setaffinity()` syscall (or the `sched_setaffinity` system call from another process). This allocates `user_cpus_ptr` on the task.

3. **Trigger one of three code paths under pi_lock**:
   - **kthread_bind path**: Call `kthread_bind()` or `kthread_bind_mask()` on a kthread that happens to have `user_cpus_ptr` set. This is unusual since kthreads rarely have user affinity set, but it is possible if `sched_setaffinity()` was called on the kthread from userspace.
   - **CPU hotplug / balance_push path**: Take a CPU offline while a task with `user_cpus_ptr` is affined to it. `__balance_push_cpu_stop()` runs on the dying CPU, acquires `pi_lock`, and calls `select_fallback_rq()` → `do_set_cpus_allowed()` to clear the task's affinity.
   - **Wakeup fallback path**: Wake a task whose allowed CPUs are all offline. `try_to_wake_up()` holds `pi_lock`, calls `select_task_rq()` → `select_fallback_rq()` → `do_set_cpus_allowed()`.

4. **SMP configuration**: The bug requires `CONFIG_SMP=y` since `user_cpus_ptr` is only used in SMP configurations.

The most common trigger is the CPU hotplug path: a user sets CPU affinity on a task via `taskset` or `sched_setaffinity()`, then one of the task's allowed CPUs is taken offline, forcing the scheduler to call `select_fallback_rq()` under `pi_lock`. The kthread_bind path is less common but possible. The bug is highly reproducible on PREEMPT_RT kernels with CPU hotplug stress testing.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

**1. PREEMPT_RT kernel requirement**: The primary manifestation of this bug — "BUG: sleeping function called from invalid context" — requires a `CONFIG_PREEMPT_RT=y` kernel. kSTEP builds and runs standard (non-RT) kernels. PREEMPT_RT fundamentally changes how `spinlock_t` and `kfree()` behave: regular spinlocks become sleeping mutexes, and memory allocator internals use sleeping locks. kSTEP's kernel build infrastructure does not support PREEMPT_RT configuration, and adding PREEMPT_RT support would be a fundamental change to the test framework, not a minor extension.

**2. CPU hotplug not supported**: The most natural trigger path requires CPU hotplug events (`__balance_push_cpu_stop()` → `select_fallback_rq()` → `do_set_cpus_allowed()`). kSTEP does not provide a `kstep_cpu_hotplug()` or similar API to take CPUs online/offline. CPU hotplug involves complex kernel-internal state machine transitions (CPU notifiers, stop_machine, migration, etc.) that would require significant framework additions to simulate.

**3. No lockdep observation mechanism**: Even if the code path could be triggered, the bug's observable consequence is lockdep warnings and `BUG()` splats in the kernel log. kSTEP's observation model is based on scheduler state inspection (`kstep_eligible()`, `kstep_output_curr_task()`, `kstep_pass()`/`kstep_fail()`) and does not provide a mechanism to detect or parse lockdep warnings from dmesg. The bug does not cause any incorrect scheduling decisions — tasks are still scheduled correctly. The only symptom is the locking correctness warning.

**4. The bug is about memory allocator interaction, not scheduling logic**: The root cause is that `kfree()` can sleep on PREEMPT_RT kernels, and the fix changes the memory deallocation strategy (using `kfree_rcu()` instead of `kfree()`). This is fundamentally about memory allocator behavior under certain locking contexts, not about scheduler decision-making or task placement. kSTEP is designed to test scheduling logic (task ordering, preemption, load balancing, EEVDF mechanics), not memory allocator/lock-ordering correctness.

**5. kthread_bind path is impractical in kSTEP**: While kSTEP can create kthreads via `kstep_kthread_create()`, there is no mechanism to set `user_cpus_ptr` on them (which requires a `sched_setaffinity()` syscall from userspace) and then bind them with `kthread_bind()`. kSTEP's `kstep_kthread_bind()` calls `kthread_bind()` internally, but the kthreads would not have `user_cpus_ptr` set, so `do_set_cpus_allowed()` would not call `kfree()` (since `ac.user_mask` would remain NULL).

**What would need to be added to kSTEP**: To reproduce this bug, kSTEP would need: (a) the ability to build and run PREEMPT_RT kernels, (b) CPU hotplug simulation (`kstep_cpu_hotplug(cpu, online/offline)`), and (c) a mechanism to detect lockdep/BUG warnings in kernel logs as pass/fail criteria. All three are fundamental architectural additions, not minor extensions.

**Alternative reproduction methods outside kSTEP**: This bug can be reproduced on a PREEMPT_RT kernel (e.g., using the `PREEMPT_RT` patch set or a kernel built with `CONFIG_PREEMPT_RT=y` on a supported version) by:
1. Setting CPU affinity on a process: `taskset -p 0x2 <pid>` (pin to CPU 1)
2. Taking that CPU offline: `echo 0 > /sys/devices/system/cpu/cpu1/online`
3. Checking dmesg for "WARNING: possible circular locking dependency detected" or "BUG: sleeping function called from invalid context"

On non-RT kernels with `CONFIG_PROVE_LOCKING=y`, the lockdep circular dependency warning can be triggered similarly, though the actual BUG() will not fire. The kernel test robot confirmed reproducibility using the Trinity system call fuzzer with `sched_setaffinity` syscalls under stress.
