# Core: do_sched_yield() Calls schedule() with Interrupts Disabled

**Commit:** `345a957fcc95630bf5535d7668a59ed983eb49a7`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.11-rc1
**Buggy since:** v2.6.12-rc2 (Fixes: 1da177e4c3f4 "Linux-2.6.12-rc2"; originally from pre-git commit a6efb709806c "[PATCH] irqlock patch 2.5.27-H6")

## Bug Description

The `do_sched_yield()` function, which implements the `sched_yield()` syscall, acquires the current CPU's runqueue lock with interrupts disabled via `this_rq_lock_irq()`. After performing the yield operation (calling `yield_task()` on the current scheduling class), it unlocks the runqueue using `rq_unlock()`, which only releases the raw spinlock but does **not** re-enable interrupts. It then proceeds to call `schedule()` with interrupts still disabled.

Calling `schedule()` with interrupts disabled is fundamentally illegal in the Linux kernel. The `__schedule()` function expects to be called with interrupts enabled (or at least to be able to manage interrupt state itself). Running the scheduler with interrupts disabled can lead to missed wakeup IPIs, timer interrupts being delayed, and on PREEMPT_RT kernels, outright warnings or deadlocks since sleeping locks replace raw spinlocks.

This bug traces back to the very earliest days of Linux kernel development — specifically to the 2.5 development series where Ingo Molnar's irqlock rework (commit a6efb709806c) restructured locking around the yield path. A misleading comment was left in the code stating: "Since we are going to call schedule() anyway, there's no need to preempt or enable interrupts," which rationalized the incorrect behavior and prevented the bug from being noticed for nearly two decades.

The fix was authored by Thomas Gleixner, a long-time kernel developer specializing in interrupt handling and real-time kernel support, who identified the issue during work on the PREEMPT_RT patchset.

## Root Cause

The root cause lies in the incorrect use of `rq_unlock()` instead of `rq_unlock_irq()` when releasing the runqueue lock in `do_sched_yield()`.

The function `this_rq_lock_irq()` (defined in `kernel/sched/sched.h`) performs two operations: (1) it calls `local_irq_disable()` to disable hardware interrupts, and (2) it acquires the runqueue lock via `rq_lock()`. The corresponding unlock function should therefore both release the lock and re-enable interrupts:

```c
// this_rq_lock_irq() — the lock acquisition:
static inline struct rq *
this_rq_lock_irq(struct rq_flags *rf)
{
    struct rq *rq;
    local_irq_disable();   // <-- disables interrupts
    rq = this_rq();
    rq_lock(rq, rf);       // <-- acquires rq->lock
    return rq;
}
```

The buggy code used `rq_unlock()` which only releases the lock without touching interrupt state:

```c
// rq_unlock() — ONLY releases the lock:
static inline void rq_unlock(struct rq *rq, struct rq_flags *rf)
{
    rq_unpin_lock(rq, rf);
    raw_spin_unlock(&rq->lock);
    // NOTE: interrupts remain disabled!
}
```

The correct function is `rq_unlock_irq()` which also re-enables interrupts:

```c
// rq_unlock_irq() — releases lock AND re-enables interrupts:
static inline void rq_unlock_irq(struct rq *rq, struct rq_flags *rf)
{
    rq_unpin_lock(rq, rf);
    raw_spin_unlock_irq(&rq->lock);  // calls local_irq_enable()
}
```

In the buggy `do_sched_yield()`, after `rq_unlock(rq, &rf)` returns, the CPU's interrupt flag remains cleared. The subsequent `sched_preempt_enable_no_resched()` call (which decrements preempt_count) and `schedule()` call both execute with interrupts disabled. The `__schedule()` function itself disables/enables interrupts internally via `local_irq_disable()` / `local_irq_enable()` around context switches, but it assumes interrupts are enabled on entry.

## Consequence

The primary consequence is that `schedule()` is called with interrupts disabled, which violates a fundamental invariant of the Linux scheduler. The observable impacts include:

1. **Latency spikes**: While interrupts are disabled from the `rq_unlock()` call through to when `__schedule()` eventually re-enables them internally, all hardware interrupts on that CPU are deferred. This includes timer interrupts, IPI wakeups from other CPUs, and device interrupts. On a system making frequent `sched_yield()` calls, this creates unpredictable latency spikes.

2. **PREEMPT_RT breakage**: On PREEMPT_RT kernels, calling `schedule()` with interrupts disabled is even more problematic because RT-mutexes (which replace regular spinlocks) may need to sleep during the scheduling path. With interrupts disabled, this leads to "BUG: scheduling while atomic" warnings and potential deadlocks.

3. **Lockdep / debug warnings**: With `CONFIG_DEBUG_ATOMIC_SLEEP` enabled, the kernel should detect that `schedule()` is being called in an atomic (interrupts-disabled) context and emit a warning. However, since the bug predated these debugging tools and the misleading comment suppressed scrutiny, it went unnoticed for years.

While the bug has existed since Linux 2.5.27 (approximately 2002), its practical impact was somewhat masked by the fact that `schedule()` internally manages interrupt state around the actual context switch. However, any code between the `rq_unlock()` and the point where `__schedule()` re-enables interrupts runs in a degraded state, and the invariant violation makes the code fragile and incorrect.

## Fix Summary

The fix is a single-line change: replacing `rq_unlock(rq, &rf)` with `rq_unlock_irq(rq, &rf)` in `do_sched_yield()`. The `rq_unlock_irq()` function calls `raw_spin_unlock_irq()` which both releases the spinlock and calls `local_irq_enable()` to re-enable hardware interrupts. This ensures that `schedule()` is called with interrupts in their proper enabled state.

Additionally, the fix removes the misleading comment that read: "Since we are going to call schedule() anyway, there's no need to preempt or enable interrupts." This comment was factually wrong — while the preemption optimization (using `preempt_disable()` / `sched_preempt_enable_no_resched()` to avoid a redundant preemption check) is valid, the claim about interrupts not needing re-enabling was incorrect and had been masking the bug.

The mailing list discussion (between Thomas Gleixner and Steven Rostedt) clarified that the `preempt_disable()` before `rq_unlock_irq()` is still intentional: it prevents an unnecessary preemption check when the lock is released, since `schedule()` is about to be called anyway. Rostedt suggested adding a revised comment to explain this optimization, but Gleixner left that decision to Peter Zijlstra.

## Triggering Conditions

The bug is triggered every single time `sched_yield()` is called by any process (or `yield()` is called from kernel code). There are no special conditions — the interrupt re-enable is always skipped on every invocation of `do_sched_yield()`.

Specific conditions to observe the consequences:
- **Any kernel version** from Linux 2.5.27 through v5.10.x (the last major release before the fix was merged in v5.11-rc1).
- **Any architecture** — the bug is architecture-independent, affecting x86, ARM, RISC-V, etc.
- **Any configuration** — the bug exists regardless of CONFIG_PREEMPT, CONFIG_SMP, or other config options, as long as the `sched_yield()` syscall is reachable.
- **No special topology** — even a single-CPU system exhibits the bug.
- **High observability** with CONFIG_DEBUG_ATOMIC_SLEEP or CONFIG_PREEMPT_RT, which make the consequences more visible through warnings or failures.

The bug is **100% deterministic** — it occurs on every `sched_yield()` call. However, the observable negative consequences (latency spikes, missed interrupts) depend on timing: whether a hardware interrupt arrives during the window when interrupts are incorrectly disabled.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why This Bug Cannot Be Reproduced with kSTEP

**Kernel version too old.** The fix was merged into v5.11-rc1, meaning the bug is already fixed in all kernel versions v5.11 and later. kSTEP supports Linux v5.15 and newer only. Since v5.15 is well past v5.11-rc1, there is no kernel version supported by kSTEP that contains this bug. Checking out `345a957fcc95~1` would yield a kernel at approximately v5.10-rc1, which is below kSTEP's v5.15 minimum.

### 2. What Would Need to Change in kSTEP

Even if the kernel version were supported, reproducing this bug would require:

- **Triggering `do_sched_yield()`**: kSTEP's `kstep_kthread_yield(p)` calls `yield()` which invokes `do_sched_yield()`, so the trigger mechanism already exists in kSTEP.
- **Observing interrupt state**: The bug is that interrupts remain disabled after `rq_unlock()`. To verify this, the driver would need to read the CPU's interrupt flag (e.g., via `local_irq_save()` / `irqs_disabled()`) immediately after the yield call returns. Since `schedule()` eventually re-enables interrupts, the observation window is inside `do_sched_yield()` itself, which is not directly hookable from a kSTEP driver. One could potentially use `on_tick_begin` callbacks to check for anomalous interrupt-disabled durations, but the bug's effect is transient within a single function.
- **Checking for debug warnings**: With `CONFIG_DEBUG_ATOMIC_SLEEP`, the kernel would emit a warning when `schedule()` is called with interrupts disabled. A kSTEP driver could check `dmesg` for such warnings after calling yield. However, this again requires the pre-v5.11 kernel.

### 3. Version Constraint

The fix (commit `345a957fcc95630bf5535d7668a59ed983eb49a7`) was merged in the v5.11-rc1 merge window. kSTEP requires v5.15 as a minimum kernel version. Therefore, no kSTEP-compatible kernel exists that contains this bug.

### 4. Alternative Reproduction Methods

To reproduce this bug outside kSTEP:

1. **Build a v5.10 kernel** with `CONFIG_DEBUG_ATOMIC_SLEEP=y` and `CONFIG_PROVE_LOCKING=y` (lockdep).
2. **Run any workload** that calls `sched_yield()` — even a simple `while(1) { sched_yield(); }` loop in userspace.
3. **Check dmesg** for warnings about "scheduling while atomic" or "BUG: sleeping function called from invalid context."
4. **For latency measurement**, use `cyclictest` or similar RT latency tools while running a yield-heavy workload, comparing against the fixed v5.11+ kernel.
5. **On PREEMPT_RT kernels** (v5.10-rt), the bug may cause more dramatic failures (deadlocks or hard lockups) due to the RT-aware sleeping locks.
