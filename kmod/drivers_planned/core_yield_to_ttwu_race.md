# Core: Race between yield_to() and try_to_wake_up() allows operation without correct rq lock

**Commit:** `5d808c78d97251af1d3a3e4f253e7d6c39fd871e`
**Affected files:** kernel/sched/syscalls.c
**Fixed in:** v6.14-rc1
**Buggy since:** v2.6.39-rc1 (commit d95f41220065 "sched: Add yield_to(task, preempt) functionality")

## Bug Description

The `yield_to()` function allows a task to yield CPU time in favor of another specific task, primarily used by KVM (`kvm_vcpu_yield_to`) to accelerate a vCPU thread toward its processor. The function works by locking both the current task's runqueue and the target task's runqueue, then calling `yield_to_task_fair()` which sets the target as the "next buddy" on its CFS runqueue (via `set_next_buddy()`) and yields the current task (via `yield_task_fair()`).

The bug is a race condition between `yield_to()` and `try_to_wake_up()`. In the buggy code, `yield_to()` only disables interrupts (`scoped_guard(irqsave)`) but does NOT acquire the target task's `pi_lock`. Meanwhile, `try_to_wake_up()` acquires `pi_lock` and can migrate the task to a different CPU via `set_task_cpu()` — critically, `set_task_cpu()` modifies `p->cpu` under `pi_lock` only, without holding the old runqueue's lock. This means `yield_to()` can end up operating on a task whose `p->cpu` (and thus `task_rq(p)`) has been changed out from under it, while `yield_to()` still holds a lock on the old (now wrong) runqueue.

The race was observed in production at Alibaba on KVM workloads, triggering a `SCHED_WARN_ON(!se->on_rq)` in `set_next_buddy()`. The warning stack trace is:
```
__warn_printk
set_next_buddy
yield_to_task_fair
yield_to
kvm_vcpu_yield_to [kvm]
```

## Root Cause

The root cause is that `yield_to()` reads `task_rq(p)` (which expands to `cpu_rq(task_cpu(p))`, reading `p->cpu`) and then locks both the local runqueue and the target's runqueue, performing a double-check `if (task_rq(p) != p_rq) goto again`. However, between the double-check passing and the actual call to `yield_to_task_fair()`, `try_to_wake_up()` on another CPU can change `p->cpu` via `set_task_cpu()` because yield_to does not hold `p->pi_lock`.

The buggy code in `yield_to()` (kernel/sched/syscalls.c, line ~1436):
```c
scoped_guard (irqsave) {       // Only disables IRQs — no pi_lock!
    rq = this_rq();
again:
    p_rq = task_rq(p);        // Read target's rq (reads p->cpu)
    ...
    guard(double_rq_lock)(rq, p_rq);  // Lock both rqs
    if (task_rq(p) != p_rq)           // Double-check
        goto again;
    ...
    yielded = curr->sched_class->yield_to_task(rq, p);  // RACE: p may have moved
}
```

The race timeline, as documented in the commit message:
```
         CPU0                             target_task

                                        blocking on CPU1
   lock rq0 & rq1
   double check task_rq == p_rq, ok
                                        woken to CPU2 (lock task_pi & rq2)
                                        task_rq = rq2
   yield_to_task_fair (w/o lock rq2)
```

In `try_to_wake_up()`, when waking a sleeping task (`on_rq == 0`), the sequence under `pi_lock` is:
1. `WRITE_ONCE(p->__state, TASK_WAKING)` — change task state
2. `set_task_cpu(p, cpu)` — write `p->cpu = new_cpu` (only under `pi_lock`, NOT under old rq's lock)
3. `ttwu_queue()` → `rq_lock(new_rq)` → `activate_task()` (enqueue, sets `se->on_rq`) → `ttwu_do_wakeup()` (sets `__state = TASK_RUNNING`)

The critical insight is that `set_task_cpu()` modifies `p->cpu` under `pi_lock` only. Since `yield_to()` does not hold `pi_lock`, the `task_rq(p)` value can change at any moment between `yield_to()`'s double-check and its call to `yield_to_task_fair()`.

When `cfs_rq_of(se)` is called inside `set_next_buddy()`, in the `!CONFIG_FAIR_GROUP_SCHED` case, it dynamically computes the CFS runqueue as `&task_rq(p)->cfs`, which reads `p->cpu` at call time. If `p->cpu` was changed by `set_task_cpu()`, this returns the NEW runqueue's CFS runqueue — but `yield_to()` holds the OLD runqueue's lock, not the new one. In the `CONFIG_FAIR_GROUP_SCHED` case, `cfs_rq_of(se)` returns `se->cfs_rq`, which is updated in `set_task_rq()` called from `set_task_cpu()` under `pi_lock`. Without `pi_lock` protection, `yield_to()` may see a partially-updated or fully-updated `se->cfs_rq` pointing to the new runqueue.

In either case, `yield_to_task_fair()` and `set_next_buddy()` end up operating on a CFS runqueue that is NOT protected by the lock `yield_to()` holds. The `se->on_rq` field, governed by enqueue/dequeue operations under the actual runqueue's lock, can be modified concurrently. This leads to `set_next_buddy()` finding `se->on_rq == 0` and firing the `SCHED_WARN_ON`.

## Consequence

The immediate observable consequence is a `SCHED_WARN_ON` kernel warning in `set_next_buddy()`, producing a stack trace and warning message in the kernel log. Since `SCHED_WARN_ON` expands to `WARN_ONCE`, the kernel continues running but the warning indicates a locking protocol violation.

The deeper consequence is a data race on the CFS runqueue's `->next` pointer. When `set_next_buddy()` writes `cfs_rq_of(se)->next = se` without holding the correct runqueue lock, this is a concurrent unsynchronized write to a data structure that other CPUs are reading/writing under their own rq lock. This can cause:
- **Stale/dangling `->next` pointer**: The CFS runqueue's `next` field points to a sched_entity that is not actually on that runqueue, potentially causing `pick_next_entity()` to select a wrong entity.
- **Priority inversion / scheduling anomaly**: The wrong task could be selected as the next task to run, violating CFS fairness guarantees.
- **Potential use-after-free**: In extreme cases, if the sched_entity is freed or reused while the `->next` pointer still references it, a use-after-free could occur, though this is unlikely in practice because `yield_to()` requires the target task struct to remain valid.

The bug was observed in KVM workloads where multiple vCPUs frequently call `kvm_vcpu_yield_to()` to yield to other vCPUs. High vCPU counts and frequent voluntary preemption increase the probability of hitting the race. On production systems, this manifests as occasional kernel warnings in dmesg, which may also trigger monitoring alerts.

## Fix Summary

The fix is a single-line change that replaces `scoped_guard(irqsave)` with `scoped_guard(raw_spinlock_irqsave, &p->pi_lock)` in `yield_to()`:

```c
// Before (buggy):
scoped_guard (irqsave) {

// After (fixed):
scoped_guard (raw_spinlock_irqsave, &p->pi_lock) {
```

This ensures that the ENTIRE `yield_to()` critical section holds the target task's `pi_lock`. Since `try_to_wake_up()` also acquires `pi_lock` before calling `set_task_cpu()`, the two functions are now serialized on the same lock. With `pi_lock` held by `yield_to()`:
- `try_to_wake_up()` cannot acquire `pi_lock` and thus cannot call `set_task_cpu()` to change `p->cpu`.
- The `task_rq(p)` value remains stable throughout `yield_to()`'s critical section.
- The double-check + yield_to_task_fair sequence is fully protected: the runqueue that was locked IS the runqueue the task belongs to.

The fix is correct and complete because `pi_lock` is the fundamental lock that serializes task migration. The `scoped_guard(raw_spinlock_irqsave, ...)` macro also disables interrupts (as part of `raw_spin_lock_irqsave`), so the interrupt-disabling behavior of the original `scoped_guard(irqsave)` is preserved. The lock ordering is safe because `pi_lock` is acquired before `rq->lock` in the standard locking hierarchy (as documented in `try_to_wake_up()` and `__task_needs_rq_lock()`).

## Triggering Conditions

The following conditions are required to trigger this bug:

- **Multiple CPUs (at least 3):** The race requires CPU0 (running `yield_to()`), the target task's current CPU (CPU1), and a third CPU (CPU2) to which the target is migrated by `try_to_wake_up()`. A fourth CPU may run the waker task.

- **CFS tasks:** Both the calling task and the target task must be in the fair scheduling class, since `yield_to_task_fair` is the only implementation of `yield_to_task`.

- **Target task transitions between sleeping and runnable:** The target task must be sleeping (or transitioning to sleep) so that `try_to_wake_up()` takes the `set_task_cpu()` path (which only runs when `on_rq == 0`). The task cannot be simply runnable on its current rq, because then `ttwu_runnable()` would handle it under the old rq's lock.

- **Concurrent `yield_to()` and `try_to_wake_up()`:** The `yield_to()` call on CPU0 and the `try_to_wake_up()` call on another CPU must overlap in time. Specifically, `set_task_cpu()` in `ttwu` must execute between `yield_to()`'s double-check (`task_rq(p) != p_rq`) and its call to `yield_to_task_fair()`.

- **Task migration to a different CPU:** `try_to_wake_up()` must select a different CPU for the target (i.e., `select_task_rq()` returns a CPU different from the current one), causing `set_task_cpu()` to change `p->cpu`.

- **No kernel configuration restriction:** The bug exists regardless of `CONFIG_FAIR_GROUP_SCHED`. Without group scheduling, `cfs_rq_of(se)` dynamically reads `task_rq(p)` (via `p->cpu`), making the stale-rq problem immediate. With group scheduling, `cfs_rq_of(se)` reads `se->cfs_rq` which is updated in `set_task_rq()` from `set_task_cpu()` under `pi_lock`.

The race probability is low per individual `yield_to()` call because the window between the double-check and `yield_to_task_fair()` is very small (just a few instructions). However, in KVM workloads with many vCPUs that frequently call `kvm_vcpu_yield_to()`, the race can be hit over time due to the sheer volume of calls. The bug is more likely on systems with many CPUs where migration is frequent.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP by creating a scenario where `yield_to()` and `try_to_wake_up()` race on the same target task. The reproduction relies on triggering the race through high-frequency concurrent operations rather than precise timing control.

### QEMU Configuration

Configure QEMU with at least 4 CPUs (CPU0 for the driver, CPU1-3 for the workload). More CPUs increase the probability of migration during wakeup, which is essential for the race.

### Task Setup

1. **Target task (T):** Create a kthread bound initially to CPU1 that repeatedly blocks and is woken. T should be a CFS (SCHED_NORMAL) task. Use `kthread_create()` directly to create a kthread with a custom function that loops: block → sleep → wake → repeat.

2. **Waker task (W):** Create a kthread on CPU2 that repeatedly calls `wake_up_process(T)` in a tight loop (with a very short delay between iterations). W drives the `try_to_wake_up()` side of the race.

3. **Yielder (driver):** The driver itself, running on CPU0 as a CFS kthread, repeatedly calls `yield_to(T, true)` in a tight loop.

### Key APIs

- Use `KSYM_IMPORT(yield_to)` to get a function pointer to `yield_to` (it's `EXPORT_SYMBOL_GPL`).
- Use standard kernel APIs: `kthread_create_on_node()`, `wake_up_process()`, `kthread_bind()`, `schedule_timeout_interruptible()`.
- Do NOT use kSTEP's `kstep_kthread_*` wrappers for the target and waker — use raw kernel thread APIs so the threads run independently without driver stepping.

### Driver Logic

```
setup:
  - Import yield_to via KSYM_IMPORT
  - Create target kthread T with function:
      while (!kthread_should_stop()) {
          set_current_state(TASK_INTERRUPTIBLE);
          schedule_timeout(1);  // Sleep for 1 jiffy
      }
  - Bind T to CPU1, start it
  - Create waker kthread W with function:
      while (!kthread_should_stop()) {
          wake_up_process(T);
          cond_resched();  // Or usleep_range(1, 10)
      }
  - Bind W to CPU2, start it

run:
  - Repeat N times (N = 100000 or more):
      ret = yield_to(T, true);
      cond_resched();
  - Stop W and T (kthread_stop)
  - Check dmesg for SCHED_WARN_ON warning
```

### Detection

The bug manifests as a `SCHED_WARN_ON(!se->on_rq)` warning in `set_next_buddy()`. Detection:
- After the test loop, read the kernel log via `printk` buffer or `dmesg`.
- Search for strings: `"WARNING"`, `"set_next_buddy"`, or `"!se->on_rq"`.
- On the **buggy kernel**: the warning should appear (possibly after many iterations). Call `kstep_fail()` if no warning is seen after all iterations, or `kstep_pass()` if the warning is found.
- On the **fixed kernel**: the warning should never appear. Call `kstep_pass()` to confirm.

### Increasing Race Probability

To maximize the chance of hitting the race:
- **Remove CPU affinity** from T after initial start, so `select_task_rq()` in `try_to_wake_up()` is free to migrate T to any CPU. This ensures `set_task_cpu()` is called frequently.
- **Use short sleep durations** for T (1 jiffy or less) to maximize the frequency of sleep → wakeup → potential-migration cycles.
- **Run many iterations** (100K+) of the yield_to loop.
- **Minimize delay** in the yield_to loop — use `cond_resched()` not `msleep()`.
- **Use 4+ CPUs** so migration targets are available.

### Expected Behavior

- **Buggy kernel (pre-fix):** After enough iterations, the kernel log should contain a `SCHED_WARN_ON` warning from `set_next_buddy`, called via `yield_to_task_fair` → `yield_to`. The race window is small, so it may take many iterations (or the test may need to run for several seconds of wall-clock time). If the warning is not observed, the test is inconclusive (not a proof of absence).

- **Fixed kernel (with fix):** `yield_to()` holds `pi_lock`, preventing `try_to_wake_up()` from concurrently migrating the task. The `set_next_buddy` function always operates with the correct rq lock held, so `se->on_rq` is stable. No warning should ever appear.

### Caveats

- This is a **probabilistic** reproduction. The race window is very small (a few instructions between the double-check and `yield_to_task_fair`). On QEMU/TCG (software CPU emulation), the race may be harder to trigger than on real hardware due to QEMU's more deterministic execution. However, with multi-threaded TCG (MTTCG, which is the default), true concurrency exists between vCPUs, making the race possible.
- If the race proves too hard to trigger through raw iteration count, consider adding a `kstep_sleep()` or `udelay()` at strategic points to increase the probability of overlap between yield_to and ttwu. For example, adding a brief busy-wait in the waker between `wake_up_process()` calls could increase the chance of the wakeup overlapping with yield_to's critical section.
- The SCHED_WARN_ON uses `WARN_ONCE`, so only the first occurrence produces a stack trace. Subsequent hits are silently ignored. Check for the warning early and terminate the test once found.
