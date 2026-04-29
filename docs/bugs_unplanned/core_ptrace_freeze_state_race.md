# Core: ptrace_freeze_traced() races with __schedule() state double-check

**Commit:** `d136122f58458479fd8926020ba2937de61d7f65`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.8-rc7
**Buggy since:** v5.8-rc6 (introduced by `dbfb089d360b` "sched: Fix loadavg accounting race")

## Bug Description

The `__schedule()` function in the Linux kernel scheduler uses a two-phase read of `prev->state` to determine whether the previous task should be deactivated (removed from the runqueue). Commit `dbfb089d360b` introduced a scheme where `prev->state` is loaded once before acquiring `rq->lock`, and then re-read after the lock is acquired with a full memory barrier (`smp_mb__after_spinlock()`). The deactivation only proceeds if both reads match: `prev_state && prev_state == prev->state`. This double-check was intended to detect when `ttwu_remote()` had changed the task's state between the two reads — if ttwu changed the state (e.g., to `TASK_WAKING` or `TASK_RUNNING`), the values wouldn't match, and deactivation would be correctly skipped.

However, this scheme implicitly assumes that only `current` (the task itself) and `ttwu()` can modify `task->state`. There is one exception to this rule: `ptrace_freeze_traced()` and `ptrace_unfreeze_traced()` in `kernel/signal.c` modify a remote task's `->state` under `siglock`. Specifically, `ptrace_freeze_traced()` changes a task's state from `TASK_TRACED` to `__TASK_TRACED` (removing the `TASK_WAKEKILL` flag) to prevent the traced task from being woken while the tracer examines it. Since `TASK_TRACED != __TASK_TRACED`, this state change causes the double-check `prev_state == prev->state` to fail, even though the task is still sleeping and should be deactivated.

As Oleg Nesterov explained in the discussion: "TASK_TRACED/TASK_STOPPED was always protected by siglock. In particular, ttwu(__TASK_TRACED) must always be called with siglock held. That is why ptrace_freeze_traced() assumes it can safely do s/TASK_TRACED/__TASK_TRACED/ under spin_lock(siglock)." This is a legitimate pattern that pre-dated the double-check mechanism, and the double-check failed to account for it.

The bug was reported by Jiri Slaby and acknowledged by Peter Zijlstra, who authored the fix. The fix was tested by Paul Gortmaker and Christian Brauner.

## Root Cause

In the buggy code path within `__schedule()`, the sequence was:

```c
/* Before acquiring rq->lock */
prev_state = prev->state;          // FIRST LOAD

rq_lock(rq, &rf);
smp_mb__after_spinlock();          // Full memory barrier

/* After acquiring rq->lock */
if (!preempt && prev_state && prev_state == prev->state) {  // SECOND LOAD
    /* ... handle deactivation ... */
    deactivate_task(rq, prev, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
}
```

The double-check `prev_state == prev->state` was the mechanism by which `__schedule()` detected interference from `ttwu_remote()`. The logic was: if the state changed between the two reads, then `ttwu_remote()` must have intervened and already handled the task, so we should skip deactivation. If the state is unchanged, then `ttwu()` did not intervene, and we proceed with deactivation.

The race occurs as follows on an SMP system:

1. **CPU A** runs `__schedule()` for a task `T` that is in `TASK_TRACED` state (it was stopped by ptrace).
2. CPU A executes the first load: `prev_state = prev->state` → gets `TASK_TRACED`.
3. **CPU B** runs `ptrace_freeze_traced()` under `siglock`, which does `WRITE_ONCE(task->state, __TASK_TRACED)`. The task's state changes from `TASK_TRACED` to `__TASK_TRACED`.
4. CPU A acquires `rq->lock` and the `smp_mb__after_spinlock()` barrier ensures visibility.
5. CPU A performs the double-check: `prev_state == prev->state` → `TASK_TRACED == __TASK_TRACED` → **false**.
6. Because the check fails, the entire `if` block is skipped. `deactivate_task()` is NOT called.

The consequence is that the task remains on the runqueue (`on_rq != 0`) despite being in a sleeping state (`__TASK_TRACED`). Additionally, the `sched_contributes_to_load` and `nr_uninterruptible` accounting that normally happens during deactivation is skipped, causing load average accounting errors.

The root cause is a violation of the assumption built into the double-check: that any state change between the two reads implies ttwu() intervention. The ptrace subsystem legitimately changes state for a remote task, and this legitimate change is indistinguishable from ttwu() interference when using a simple equality check.

The ordering requirement that the double-check was trying to satisfy was: the `prev->state` load must be ordered before the `prev->on_rq = 0` store (inside `deactivate_task()`). This LOAD→STORE ordering ensures that `ttwu()` on another CPU, which checks `p->on_rq` before `p->state`, sees a consistent view. The original approach used `rq->lock` + `smp_mb__after_spinlock()` + equality re-check to establish this ordering. The fix achieves the same ordering more simply via a control dependency.

## Consequence

The primary observable consequence is **incorrect load average accounting**. When `__schedule()` skips deactivation due to the ptrace race, the task's `sched_contributes_to_load` flag is never set, and `rq->nr_uninterruptible` is never incremented (or equivalently, the decrement in `ttwu_do_activate()` occurs without a matching increment). This causes the global `calc_load_tasks` counter to "leak" — it drifts from its correct value over time.

In the real-world reports from the LKML thread, Dave Jones observed that on a mostly idle firewall machine running kernel 5.8-rc2/rc3, load average would creep up from its normal 0.xx range to above 1.00 and never drop back. Over hours, it climbed to 7.xx. Paul Gortmaker confirmed the same behavior using RCU torture tests — after hours of runtime, `calc_load_tasks` had a non-zero counter (`{counter = 1}`) even when no tasks were actually running or blocked. Manually resetting `calc_load_tasks` to zero via GDB caused load average to decay back to normal, confirming the accounting leak.

The bug is non-deterministic and requires the ptrace state change to race with `__schedule()` in a very narrow window (between the first `prev->state` load before `rq_lock` and the re-check after the lock). The race is rare per occurrence but accumulates over time with any workload that involves ptrace (e.g., `strace`, debuggers, container runtimes that use ptrace, or periodic cron jobs). Each occurrence leaks one unit into `calc_load_tasks`, and since load average uses exponential decay, leaked units persist for a long time. The bug does not cause crashes, hangs, or incorrect scheduling decisions — only inflated load average numbers. However, this can affect monitoring, autoscaling, and capacity planning systems that rely on load average.

## Fix Summary

The fix replaces the two-phase load-and-recheck approach with a single load and a control dependency. The key change in `__schedule()` is:

**Before (buggy):**
```c
prev_state = prev->state;          // Load BEFORE rq_lock
rq_lock(rq, &rf);
smp_mb__after_spinlock();
if (!preempt && prev_state && prev_state == prev->state)  // Recheck after lock
    deactivate_task();
```

**After (fixed):**
```c
rq_lock(rq, &rf);
smp_mb__after_spinlock();
prev_state = prev->state;          // Single load AFTER rq_lock
if (!preempt && prev_state)         // No recheck; control dependency suffices
    deactivate_task();
```

The `prev->state` load is moved to after the `rq_lock()` and `smp_mb__after_spinlock()`, and the equality re-check is removed. The `if (prev_state)` branch creates a **control dependency** on the loaded value: the CPU cannot speculatively execute `deactivate_task()` (which performs `prev->on_rq = 0`) until the `prev->state` load completes. This control dependency provides the required LOAD(`prev->state`) → STORE(`prev->on_rq = 0`) ordering without needing a separate memory barrier or double-check.

The fix is correct and complete because: (1) the control dependency guarantees that the `on_rq` store is ordered after the state load, which is the only ordering requirement for the ttwu() interaction; (2) a single read of `prev->state` (which is `volatile`) captures whatever state the task is currently in — if ptrace changed it from `TASK_TRACED` to `__TASK_TRACED`, both are non-zero, so deactivation correctly proceeds; (3) if `ttwu_remote()` changed the state to `TASK_RUNNING` (zero), the `if (prev_state)` check correctly skips deactivation. The updated comment in the code also documents that `ptrace_{,un}freeze_traced()` can change `->state` underneath the scheduler.

## Triggering Conditions

The following conditions must all be met to trigger the bug:

- **Kernel version**: Must be running a kernel with commit `dbfb089d360b` but without `d136122f5845`. This window is v5.8-rc6 only (the bug was introduced in rc6 and fixed in rc7).
- **SMP system**: At least 2 CPUs are required. One CPU runs `__schedule()` for the traced task, while another CPU runs `ptrace_freeze_traced()`.
- **Ptrace activity**: A tracer process must be attached to a tracee, and the tracee must be in `TASK_TRACED` state when `__schedule()` runs. The tracer must call a ptrace operation that invokes `ptrace_freeze_traced()` (e.g., `ptrace(PTRACE_PEEKDATA, ...)` or any request that goes through `ptrace_check_attach()` → `ptrace_freeze_traced()`).
- **Precise timing**: The `ptrace_freeze_traced()` call on CPU B must execute between the first `prev_state = prev->state` load (before `rq_lock`) and the re-check `prev->state` read (after `rq_lock`) on CPU A. This is a very narrow window — only a few instructions wide.
- **Workload that involves ptrace**: Any workload using `strace`, GDB, or container runtimes that ptrace child processes can trigger this. The bug accumulates over time since each race occurrence leaks one unit into `calc_load_tasks`. Workloads with frequent short-lived ptrace sessions (like periodic `strace` invocations from cron) maximize the chance of hitting the race.

The race is probabilistically rare per-attempt but compounds over time. Multiple reporters confirmed it takes hours of uptime before the load average drift becomes noticeable. The probability increases with more CPUs and higher ptrace activity.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **KERNEL VERSION TOO OLD**: The bug was introduced by commit `dbfb089d360b` (merged in v5.8-rc6) and fixed by commit `d136122f5845` (merged in v5.8-rc7). The buggy code existed only in the v5.8-rc6 to v5.8-rc6 kernel window. kSTEP supports Linux v5.15 and newer only, and by v5.15 the fix has been present for over a year. There is no kernel version ≥ v5.15 where this bug exists. Running `checkout_linux.py` with the buggy parent commit (`d136122f~1`) would check out a v5.8-rc6 kernel, which is far below kSTEP's minimum supported version.

2. **Requires ptrace userspace operations**: Even if the kernel version were compatible, the bug requires ptrace operations from a real userspace process. Specifically, a tracer process must call `ptrace(PTRACE_*)` to trigger `ptrace_check_attach()` → `ptrace_freeze_traced()`, which changes the tracee's `task->state` from `TASK_TRACED` to `__TASK_TRACED`. kSTEP operates through kernel modules controlling kernel threads (`kthread`). Kernel threads cannot be ptraced (they have no userspace context), and kSTEP has no mechanism to create userspace tracer/tracee process pairs or invoke ptrace syscalls.

3. **Extremely narrow race window**: The race window is between two consecutive reads of `prev->state` in `__schedule()`, separated only by the `rq_lock()` acquisition and `smp_mb__after_spinlock()`. This is on the order of tens of nanoseconds. kSTEP's timing control granularity is tick-based (millisecond-level via `kstep_tick()`), which is far too coarse to reliably hit this window. No existing kSTEP callback (e.g., `on_tick_begin`, `on_sched_softirq_begin`) fires between these two reads.

4. **What would need to be added to kSTEP**: To reproduce this bug, kSTEP would need: (a) support for creating real userspace processes (not kthreads) with ptrace relationships; (b) a `kstep_ptrace_attach(tracer, tracee)` and `kstep_ptrace_freeze(tracee)` API to invoke ptrace operations; (c) instruction-level timing control to position the ptrace operation within the narrow race window in `__schedule()`; (d) support for building and running v5.8-rc6 kernels (well below the v5.15 minimum). These are fundamental architectural changes, not minor extensions.

5. **Alternative reproduction**: Outside kSTEP, this bug could be reproduced on a real v5.8-rc6 kernel by running a workload that frequently creates short-lived ptrace sessions. For example, running `strace` on many short-lived processes in a loop on a multi-CPU system should eventually trigger the race. Monitoring `cat /proc/loadavg` over several hours would reveal the symptom (load average creeping up on an idle system). The GDB approach from the mailing list (checking `calc_load_tasks` counter) provides a more direct diagnostic. However, reproduction is inherently non-deterministic and may take hours.
