# Core: RQCF_ACT_SKIP Flag Leak from __schedule() to Remote rq Lock Holder

**Commit:** `5ebde09d91707a4a9bec1e3d213e3c12ffde348f`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.7-rc1
**Buggy since:** v6.5-rc1 (introduced by commit `ebb83d84e49b` "sched/core: Avoid multiple calling update_rq_clock() in __cfsb_csd_unthrottle()")

## Bug Description

The `RQCF_ACT_SKIP` flag in `rq->clock_update_flags` is used to skip redundant `update_rq_clock()` calls during scheduling. In `__schedule()`, the flag lifecycle is: (1) promote `RQCF_REQ_SKIP` to `RQCF_ACT_SKIP` via `rq->clock_update_flags <<= 1`, (2) call `update_rq_clock()` which uses the flag to skip the update, and then (3) clear the SKIP flags much later — either in `context_switch()` after `switch_mm_cid()`, or in the else-branch when no context switch occurs (prev == next).

The problem is that between steps (2) and (3), there is a large window during which `RQCF_ACT_SKIP` remains set in `rq->clock_update_flags`. During this window, `pick_next_task_fair()` may call `newidle_balance()`, which drops and re-acquires the runqueue lock. While the lock is dropped, another CPU can acquire this runqueue's lock and observe the stale `RQCF_ACT_SKIP` flag.

Commit `ebb83d84e49b` introduced `rq_clock_start_loop_update()` which contains a `SCHED_WARN_ON(rq->clock_update_flags & RQCF_ACT_SKIP)` check. When the leaked flag is observed by this function (called from `__cfsb_csd_unthrottle()` or `unthrottle_offline_cfs_rqs()`), a kernel WARNING fires. A secondary warning (`rq->clock_update_flags < RQCF_ACT_SKIP`) may also fire in `update_curr()` as the stale flag interferes with the `assert_clock_updated()` check.

This regression was reported by multiple users on production systems running kernels 6.4.11 and 6.4.12 (which backported `ebb83d84e49b`), manifesting as kernel warnings during cgroup destruction or CFS bandwidth unthrottling under concurrent scheduling activity.

## Root Cause

The root cause is a timing gap in `__schedule()` in `kernel/sched/core.c`. The relevant code flow (before the fix) is:

```c
// In __schedule():
rq_lock(rq, &rf);
smp_mb__after_spinlock();

/* Promote REQ to ACT */
rq->clock_update_flags <<= 1;   // RQCF_REQ_SKIP (0x01) becomes RQCF_ACT_SKIP (0x02)
update_rq_clock(rq);            // Uses ACT_SKIP to skip the update

// ... many lines of code including pick_next_task() ...
// pick_next_task_fair() may call newidle_balance() which DROPS the rq lock

if (likely(prev != next)) {
    // ... context_switch() eventually clears:
    rq->clock_update_flags &= ~(RQCF_ACT_SKIP|RQCF_REQ_SKIP);  // TOO LATE
} else {
    rq->clock_update_flags &= ~(RQCF_ACT_SKIP|RQCF_REQ_SKIP);  // ALSO TOO LATE
}
```

The `RQCF_ACT_SKIP` flag is set at the `<<= 1` operation and only cleared in `context_switch()` (line ~5363) or the no-switch else-branch (line ~6677). Between these points, `newidle_balance()` can drop and re-acquire the runqueue lock, creating a window for another CPU to observe the stale flag.

The specific race interleaving (from Peter Zijlstra's analysis) is:

```
CPU0                                      CPU1

__schedule()
  rq->clock_update_flags <<= 1;   unregister_fair_sched_group()
    (ACT_SKIP now set)              destroy_cfs_bandwidth()
  update_rq_clock(rq);
  pick_next_task_fair()
    newidle_balance()
      rq_unpin_lock(this_rq, rf)
        // rf->clock_update_flags = RQCF_UPDATED saved
      raw_spin_rq_unlock(this_rq)
                                    for_each_possible_cpu(i) [i=CPU0]
                                      __cfsb_csd_unthrottle()
                                        rq_lock(CPU0_rq, &rf)
                                          rq_pin_lock()
                                            // preserves ACT_SKIP:
                                            // rq->clock_update_flags &= (REQ_SKIP|ACT_SKIP)
                                        update_rq_clock(rq)
                                        rq_clock_start_loop_update(rq)
                                          WARN_ON(rq->clock_update_flags & RQCF_ACT_SKIP) ← SPLAT
      raw_spin_rq_lock(this_rq)
        rq_repin_lock()
          // restores RQCF_UPDATED, but clock may be stale
```

The key issue is that `rq_pin_lock()` preserves the SKIP flags (`rq->clock_update_flags &= (RQCF_REQ_SKIP|RQCF_ACT_SKIP)`), so CPU1's lock acquisition does not clear CPU0's stale `RQCF_ACT_SKIP`. When `rq_clock_start_loop_update()` then checks for the flag, it finds it set and triggers the warning.

## Consequence

The primary consequence is a kernel `WARN_ON_ONCE` firing in `rq_clock_start_loop_update()` at `kernel/sched/sched.h:1561`. The full warning message is:

```
rq->clock_update_flags & RQCF_ACT_SKIP
WARNING: CPU: 13 PID: 3837105 at kernel/sched/sched.h:1561 __cfsb_csd_unthrottle+0x149/0x160
```

A secondary warning may also fire in `update_curr()`:

```
rq->clock_update_flags < RQCF_ACT_SKIP
WARNING: CPU: 0 PID: 3920513 at kernel/sched/sched.h:1496 update_curr+0x162/0x1d0
```

The typical call trace shows the warning triggered from `unregister_fair_sched_group()` → `destroy_cfs_bandwidth()` → `__cfsb_csd_unthrottle()`, during an RCU callback (`rcu_do_batch`) processing cgroup destruction. The stack trace from the Bugzilla report (Bug #217843) shows this happening during a soft IRQ context triggered by a timer interrupt.

Beyond the warning itself, there is a subtle correctness issue: when CPU1 takes CPU0's rq lock and sees the stale `RQCF_ACT_SKIP`, the `rq_clock_start_loop_update()` function will OR in `RQCF_ACT_SKIP` on top of it. Later, when CPU0 re-pins the lock via `rq_repin_lock()`, it restores `RQCF_UPDATED` from `rf->clock_update_flags`, but the actual rq clock may have moved on significantly since the original update. This means subsequent code on CPU0 may use a stale rq clock value, though the practical impact is small since clock updates happen frequently.

The bug was reported on production systems with 13+ CPUs running container workloads (OpenStack Compute), where cgroup creation/destruction is frequent. The `W` taint flag indicates prior warnings. While not a crash, the warnings pollute kernel logs and indicate internal scheduler state inconsistency.

## Fix Summary

The fix in commit `5ebde09d91707a4a9bec1e3d213e3c12ffde348f` takes Peter Zijlstra's suggested approach: clear the SKIP flags immediately after `update_rq_clock()` rather than waiting until `context_switch()` or the no-switch branch. Specifically, the fix:

1. **Adds** `rq->clock_update_flags = RQCF_UPDATED;` immediately after the `update_rq_clock(rq)` call in `__schedule()` (line ~6601). This assignment replaces the entire flags value with `RQCF_UPDATED` (0x04), which implicitly clears both `RQCF_ACT_SKIP` (0x02) and `RQCF_REQ_SKIP` (0x01), and marks the clock as freshly updated.

2. **Removes** `rq->clock_update_flags &= ~(RQCF_ACT_SKIP|RQCF_REQ_SKIP);` from two locations:
   - In `context_switch()` after `switch_mm_cid()` (line ~5363), since the flags are already cleared.
   - In the `prev == next` else-branch of `__schedule()` (line ~6677), since the flags are already cleared.

This fix is correct because the sole purpose of `RQCF_ACT_SKIP` is to skip the `update_rq_clock()` call at the top of `__schedule()`. Once that call completes, the flag has served its purpose and should be cleared immediately. There is no reason to keep it set through the rest of the scheduling path. Setting the flags to `RQCF_UPDATED` after `update_rq_clock()` both clears the stale SKIP flags and correctly indicates that the clock has been updated, preventing the `rq->clock_update_flags < RQCF_ACT_SKIP` warning that might otherwise fire in subsequent `assert_clock_updated()` checks.

## Triggering Conditions

The bug requires the following precise conditions to trigger:

1. **At least 2 CPUs**: One CPU (CPU0) must be running `__schedule()` and reaching `newidle_balance()`, while another CPU (CPU1) must be destroying a CFS bandwidth group and locking CPU0's runqueue.

2. **CFS bandwidth throttling active**: A task group with CFS bandwidth limits (`cpu.cfs_quota_us`/`cpu.cfs_period_us` in cgroup v1, or `cpu.max` in cgroup v2) must be configured and active. Tasks in this group must have been throttled at some point so that the bandwidth infrastructure is in use.

3. **Cgroup destruction concurrent with scheduling**: The cgroup containing the bandwidth-limited task group must be destroyed (triggering `unregister_fair_sched_group()` → `destroy_cfs_bandwidth()`) at the exact moment that CPU0 is in the `newidle_balance()` code path with its runqueue lock dropped.

4. **CPU0 must enter newidle_balance()**: CPU0's runqueue must become empty (all tasks dequeued or migrated), causing `pick_next_task_fair()` to return NULL and trigger `newidle_balance()`. During newidle_balance, the runqueue lock is dropped (`raw_spin_rq_unlock()`) to allow pulling tasks from other CPUs.

5. **Timing-critical race window**: CPU1 must acquire CPU0's runqueue lock during the narrow window between `raw_spin_rq_unlock()` in `newidle_balance()` and `raw_spin_rq_lock()` that re-acquires it. This window is typically very small (microseconds), making the race probabilistic rather than deterministic.

6. **RQCF_REQ_SKIP must have been set before __schedule()**: For `RQCF_ACT_SKIP` to exist, `rq_clock_skip_update()` must have been called before `__schedule()`, setting `RQCF_REQ_SKIP` which then gets promoted to `RQCF_ACT_SKIP` by the `<<= 1` operation.

The bug was observed on production systems with many CPUs (13+), running container workloads with frequent cgroup creation/destruction (e.g., OpenStack Compute environments). The probability increases with more CPUs and more frequent cgroup lifecycle events.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reliably reproduced with kSTEP for the following reasons:

### 1. Why this bug cannot be reproduced with kSTEP

The fundamental challenge is that this bug requires a **cross-CPU race condition** with extremely precise timing that kSTEP cannot orchestrate deterministically:

- **The race window is inherently non-deterministic**: CPU0 must be inside `newidle_balance()` with its runqueue lock dropped, while CPU1 must simultaneously be destroying a CFS bandwidth group and iterating to CPU0's runqueue. The lock-drop window in `newidle_balance()` lasts only microseconds, and kSTEP has no mechanism to pause execution at that exact point to force the race.

- **kSTEP lacks CFS bandwidth cgroup configuration**: kSTEP provides `kstep_cgroup_create()` and `kstep_cgroup_set_weight()`, but has no API to set CFS bandwidth limits (`cpu.cfs_quota_us`/`cpu.cfs_period_us`). Without bandwidth throttling, the `__cfsb_csd_unthrottle()` code path cannot be exercised.

- **kSTEP lacks cgroup destruction**: There is no `kstep_cgroup_destroy()` API. The bug is triggered during cgroup teardown (`unregister_fair_sched_group()` → `destroy_cfs_bandwidth()`), which requires removing the cgroup. kSTEP can only create cgroups, not destroy them.

- **kSTEP operates single-threaded**: While kSTEP can create kthreads on different CPUs, the driver itself executes sequentially. Orchestrating the precise interleaving where one CPU is mid-`newidle_balance()` while another is destroying a cgroup requires true concurrent execution with exact timing control that kSTEP's sequential model cannot provide.

- **The bug manifests as a WARN_ON_ONCE, not scheduling behavioral change**: kSTEP's detection mechanisms (`kstep_pass`/`kstep_fail`, `kstep_eligible`, `kstep_output_curr_task`, etc.) are designed to observe scheduling behavior. While one could check `rq->clock_update_flags` directly via internal access, triggering the stale flag state requires the race to actually occur.

### 2. What would need to be added to kSTEP

To potentially support this bug, kSTEP would need:

- **`kstep_cgroup_set_bandwidth(name, quota_us, period_us)`**: Write to `cpu.cfs_quota_us` and `cpu.cfs_period_us` (cgroup v1) or `cpu.max` (cgroup v2) to configure CFS bandwidth limits.
- **`kstep_cgroup_destroy(name)`**: Trigger cgroup removal which calls `css_free_rwork_fn()` → `sched_unregister_group_rcu()` → `unregister_fair_sched_group()` → `destroy_cfs_bandwidth()`.
- **A mechanism to inject code at the `newidle_balance()` lock-drop point**: Something like `on_newidle_balance_unlock` callback that fires when a CPU drops its rq lock during newidle balance. This would allow synchronizing the race.
- Even with all of the above, reproducing the race deterministically would likely require **spinlock instrumentation** or a **delay injection mechanism** that holds CPU0 in the unlocked window long enough for CPU1 to acquire the lock — a fundamental architectural change to kSTEP.

### 3. Alternative reproduction methods

- **Stress testing on real hardware**: Run a container orchestration workload (e.g., rapidly creating and destroying cgroups with bandwidth limits) on a multi-CPU system. The bug was reported on systems with 13+ CPUs under OpenStack. The `WARN_ON_ONCE` will fire in dmesg.
- **Kernel instrumentation**: Add a deliberate `udelay()` or `mdelay()` in `newidle_balance()` after dropping the lock but before re-acquiring it, to widen the race window. Then trigger cgroup destruction concurrently.
- **Lockdep/KCSAN**: Compile with `CONFIG_KCSAN` or custom annotations to detect the data race on `rq->clock_update_flags` across CPUs.
- **Syzkaller**: A fuzzer like syzkaller configured to rapidly create/destroy cgroups with bandwidth limits while scheduling tasks could probabilistically hit the race.
