# Core: Loadavg Accounting Race in ttwu() vs schedule()

**Commit:** `dbfb089d360b1cc623c51a2c7cf9b99eff78e0e7`
**Affected files:** `kernel/sched/core.c`, `include/linux/sched.h`
**Fixed in:** v5.8-rc6
**Buggy since:** v5.8-rc1 (introduced by `c6e7bd7afaeb` "sched/core: Optimize ttwu() spinning on p->on_cpu")

## Bug Description

The Linux kernel's load average calculation depends on correct tracking of tasks in the `TASK_UNINTERRUPTIBLE` state via the per-runqueue counter `rq->nr_uninterruptible`. When a task blocks in `TASK_UNINTERRUPTIBLE` state, `nr_uninterruptible` is incremented; when it wakes up, it is decremented. This counter feeds into the global `calc_load_tasks` atomic, which drives the system-wide load average reported by `/proc/loadavg`.

Commit `c6e7bd7afaeb` optimized `try_to_wake_up()` (ttwu) by moving the evaluation of `p->sched_contributes_to_load` and the `p->state = TASK_WAKING` assignment earlier in the wakeup path — from after the `smp_cond_load_acquire(&p->on_cpu, !VAL)` spin-wait into the `p->on_rq == 0` block. The rationale was that once a task enters `schedule()`, it cannot change its own `->state` anymore, so it should be safe to read `p->state` earlier. However, this reasoning was both incorrect and flawed.

The first problem is a memory ordering issue: reading `p->state` after observing `p->on_rq == 0` requires at least an ACQUIRE barrier on the `on_rq` load to prevent weak-memory-model hardware from reordering the state read before the `on_rq` observation. Without this, on architectures like ARM, the `p->state` load could be speculatively hoisted above the `p->on_rq` check, yielding stale or transient state values.

The second and more fundamental problem is that while `schedule()` does not *write* `prev->state`, it *reads* `prev->state` multiple times (the field is declared `volatile`). The old guard `p->on_cpu == 0` ensured `schedule()` had fully completed and would never reference `prev->state` again. The new earlier point `p->on_rq == 0` does not guarantee this — `schedule()` may still be running on the remote CPU, reading `prev->state` for its own dequeue decision, while ttwu on another CPU overwrites it to `TASK_WAKING`. This race corrupts the load average accounting because `sched_contributes_to_load` is evaluated against a state value that `schedule()` may simultaneously be using for its own `nr_uninterruptible` tracking.

## Root Cause

In the buggy code, `try_to_wake_up()` executes the following sequence in the `p->on_rq == 0` (SMP) path, *before* waiting for `p->on_cpu == 0`:

```c
p->sched_contributes_to_load = !!task_contributes_to_load(p);
p->state = TASK_WAKING;
```

Meanwhile, `__schedule()` on the remote CPU is still executing after setting `p->on_rq = 0` (inside `deactivate_task()`). The `__schedule()` function reads `prev->state` to decide whether to dequeue the task and whether to increment `rq->nr_uninterruptible` (via `task_contributes_to_load()` in `activate_task()`/`deactivate_task()`). In the original code, the `task_contributes_to_load()` macro was evaluated inside `activate_task()` and `deactivate_task()`.

The race unfolds as follows:

1. **CPU A** runs task P, which calls `schedule()` after setting `current->state = TASK_UNINTERRUPTIBLE`.
2. **CPU A** in `__schedule()`: acquires `rq->lock`, reads `prev->state` (sees `TASK_UNINTERRUPTIBLE`), calls `deactivate_task()` which sets `p->on_rq = 0` and increments `rq->nr_uninterruptible` (task contributes to load).
3. **CPU B** calls `try_to_wake_up(P)`: acquires `P->pi_lock`, observes `P->state & state` match, does `smp_rmb()`, reads `P->on_rq == 0` (the dequeue happened).
4. **CPU B** (buggy code): immediately evaluates `p->sched_contributes_to_load = !!task_contributes_to_load(p)`. This reads `p->state` which is *still* `TASK_UNINTERRUPTIBLE`. Then sets `p->state = TASK_WAKING`.
5. **CPU A** continues `__schedule()`: `schedule()` may still read `prev->state` for signal checks or other volatile accesses. Since `p->state` was overwritten to `TASK_WAKING` by CPU B, the state used by schedule's remaining logic can be inconsistent.

The critical accounting issue: `sched_contributes_to_load` was set to `true` by ttwu (step 4), and `deactivate_task()` already incremented `rq->nr_uninterruptible` (step 2). When `ttwu_do_activate()` runs, it decrements `rq->nr_uninterruptible` because `p->sched_contributes_to_load == true`. This *seems* balanced, but the race can cause a mismatch. If `ttwu_remote()` handles the wakeup (task is still on_rq because the dequeue hasn't completed yet due to weak ordering), neither increment nor decrement happen correctly. Or, if schedule() on CPU A re-reads `prev->state` after ttwu overwrites it to `TASK_WAKING`, it may take the wrong branch in the blocking path (e.g., not decrementing because the state no longer looks uninterruptible). Over many iterations, these mismatches cause `nr_uninterruptible` to drift, leaking phantom load into `calc_load_tasks`.

## Consequence

The observable consequence is a monotonically increasing system load average on an otherwise idle machine. Users reported load averages climbing from near-zero to 1.0, then 7.0, then higher over hours or days of uptime, despite the system being essentially idle. The load average never recovers once corrupted — the counter `calc_load_tasks` leaks upward and stays elevated permanently.

Dave Jones reported: "When I upgraded my firewall to 5.8-rc2 I noticed that on a mostly idle machine (that usually sees loadavg hover in the 0.xx range) that it was consistently above 1.00 even when there was nothing running. [...] One morning I woke up to find loadavg at '7.xx', after almost as many hours of uptime." Paul Gortmaker independently confirmed the same behavior and demonstrated it was an accounting leak by manually resetting `calc_load_tasks` to 0 via GDB, after which load average decayed back to normal — proving the issue was a counter imbalance, not actual load.

The bug does not cause crashes, hangs, or data corruption, but it renders the system load average metric completely unreliable. This affects system monitoring, autoscaling, load-balancing decisions, and any tool that relies on `/proc/loadavg`. The race is probabilistic and requires many TASK_UNINTERRUPTIBLE wakeup cycles to manifest visibly, which is why it took weeks to bisect and hours of runtime to confirm.

## Fix Summary

The fix restructures the load average accounting to eliminate the race between `__schedule()` and `try_to_wake_up()`. The key changes are:

1. **Move `sched_contributes_to_load` computation and `nr_uninterruptible` increment into `__schedule()` itself**, right before `deactivate_task()`. Previously, `activate_task()` decremented and `deactivate_task()` incremented `nr_uninterruptible` using the live `task_contributes_to_load()` macro. Now, `__schedule()` computes `prev->sched_contributes_to_load` from a locally-cached `prev_state` variable and increments `rq->nr_uninterruptible` before calling `deactivate_task()`. The `task_contributes_to_load()` macro is removed entirely from `include/linux/sched.h`, replaced by inline logic in `__schedule()`.

2. **Cache `prev->state` before acquiring `rq->lock`** (`prev_state = prev->state`). After acquiring `rq->lock`, the code re-checks `prev_state == prev->state`. If the state changed (because `ttwu_remote()` set it to `TASK_RUNNING`), the blocking path is skipped entirely. This ensures `__schedule()` uses a stable state value and is immune to concurrent ttwu state modifications.

3. **Move `p->state = TASK_WAKING` after the `smp_acquire__after_ctrl_dep()` barrier** in ttwu, which replaces the weaker `smp_rmb()`. The `smp_acquire__after_ctrl_dep()` forms a control-dependency-acquire with the `p->on_rq == 0` check above, ensuring that `schedule()`'s `deactivate_task()` has truly completed and `schedule()` will no longer read `prev->state`. Only then does ttwu write `TASK_WAKING`.

4. **Move the `nr_uninterruptible--` decrement in `ttwu_do_activate()` outside the `#ifdef CONFIG_SMP` block**, making it unconditional. The increment in `__schedule()` is also unconditional.

5. **Use `READ_ONCE(p->on_rq)` in ttwu** for the `p->on_rq` check to prevent compiler-induced issues and to clearly mark the data-race-aware load.

Together these changes establish the following memory ordering guarantee:

```
__schedule() (CPU A)              ttwu() (CPU B)
  LOAD prev->state                  LOAD-ACQUIRE p->on_rq == 0
  MB (rq->lock acquire)
  STORE p->on_rq = 0               STORE p->state = TASK_WAKING
```

This ensures the `TASK_WAKING` store on CPU B happens only after CPU A's `prev->state` load, so `schedule()` always sees the original state value it cached before `rq->lock`.

## Triggering Conditions

- **Kernel version:** v5.8-rc1 through v5.8-rc5 (only kernels containing `c6e7bd7afaeb` but not `dbfb089d360b`).
- **SMP required:** The bug is a cross-CPU race between `__schedule()` and `try_to_wake_up()`. At least 2 CPUs are needed.
- **Workload:** Frequent TASK_UNINTERRUPTIBLE sleeps followed by wakeups. The reporters saw it with cron jobs spawning thousands of short-lived processes (involving many iptables/ipset invocations) and RCU torture testing. Any workload with high churn of blocking/waking tasks increases the probability.
- **Weak memory ordering:** The ordering bug is more likely to manifest on weakly-ordered architectures (ARM, POWER), but was also observed on x86 due to the second aspect of the bug (volatile re-reads of `prev->state` in `__schedule()`).
- **Time:** The race is rare per-occurrence. It takes hours to days for the cumulative counter drift to become noticeable. The reporters needed 2-7 hours of runtime to reliably distinguish buggy from fixed kernels.
- **No special kernel configuration:** The bug affects default SMP kernel configurations. No special CONFIG options are needed beyond having SMP enabled.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **KERNEL VERSION TOO OLD:** The fix commit `dbfb089d360b` was merged into **v5.8-rc6**. The bug was introduced in **v5.8-rc1** by commit `c6e7bd7afaeb`. kSTEP supports Linux **v5.15 and newer only**. Both the buggy and fixed kernel versions are far older than the v5.15 minimum. There is no kernel version that kSTEP can build where this bug exists — the fix has been present since v5.8-rc6, years before v5.15.

2. **Alternative reproduction outside kSTEP:** The bug can be reproduced on bare metal or in a standard QEMU/KVM VM running a v5.8-rc1 through v5.8-rc5 kernel, under a workload that generates many TASK_UNINTERRUPTIBLE wakeups (e.g., heavy I/O, process spawning storms, or RCU torture tests). Monitor `/proc/loadavg` over several hours; on affected kernels, the load average will drift upward on an otherwise idle system. Paul Gortmaker's test used `tools/testing/selftests/rcutorture/bin/kvm.sh --cpus 24 --duration 120 --configs TREE03 --trust-make` and needed 2+ hours to see the effect. After the test completes, if load average does not decay back to near-zero within 15-30 minutes, the bug is present.

3. **Verification via GDB:** On the buggy kernel, one can attach a debugger and check `calc_load_tasks`. If its value is positive when no tasks should be in TASK_UNINTERRUPTIBLE state, the accounting has drifted. Resetting it to 0 and observing load average decay confirms the leak.

4. **Why minor kSTEP extensions would not help:** Even if kSTEP could be extended with additional APIs, the fundamental blocker is the kernel version constraint. The code paths involved (`__schedule()`, `try_to_wake_up()`, `activate_task()`, `deactivate_task()`) were substantially restructured by this fix and subsequent patches. On v5.15+, the fix is already integrated and the race does not exist.
