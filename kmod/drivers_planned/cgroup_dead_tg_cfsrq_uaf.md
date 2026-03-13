# Cgroup: Dead Task Group Regains cfs_rq's Leading to Use-After-Free

**Commit:** `b027789e5e50494c2325cc70c8642e7fd6059479`
**Affected files:** `kernel/sched/autogroup.c`, `kernel/sched/core.c`, `kernel/sched/fair.c`, `kernel/sched/rt.c`, `kernel/sched/sched.h`
**Fixed in:** v5.16-rc1
**Buggy since:** `a7b359fc6a37` ("sched/fair: Correctly insert cfs_rq's to list on unthrottle"), merged in v5.13-rc7

## Bug Description

When a cgroup with CFS bandwidth throttling is being destroyed, there is a race between the cgroup teardown path (which unlinks `cfs_rq`'s from the dying task group) and the CFS bandwidth period timer (which walks the task group hierarchy and re-adds `cfs_rq`'s to task groups during unthrottling). This race results in freed `cfs_rq` objects remaining linked on the per-rq leaf list, causing a use-after-free when `update_blocked_averages()` later iterates over that list.

The root of the problem is commit `a7b359fc6a37` which changed `tg_unthrottle_up()` to re-add `cfs_rq`'s to the leaf list even when they have no running tasks — specifically when the `cfs_rq` has non-decayed load (`!cfs_rq_is_decayed(cfs_rq)`). Before that commit, only `cfs_rq`'s with `nr_running > 0` would be re-added. Since cgroups can only be removed when they have no tasks, the old code was safe: no tasks meant `nr_running == 0`, so `cfs_rq`'s would never be re-added to a dying task group. After `a7b359fc6a37`, transient residual load (which takes time to decay) provides an alternate condition for re-adding, and this condition can be true for dying task groups.

The pre-fix cgroup teardown sequence was: `sched_offline_group()` first calls `unregister_fair_sched_group()` (which unlinks all `cfs_rq`'s via `list_del_leaf_cfs_rq()`), then unlinks the task group from the global `task_groups` list and parent's `children` list via `list_del_rcu()`. This order is exactly backwards for safety: by the time the task group is removed from the hierarchy, its `cfs_rq`'s may have already been re-added by a concurrent timer running `tg_unthrottle_up()`.

The UAF manifests when `update_blocked_averages()` (called from the scheduler softirq `run_rebalance_domains()` or from `newidle_balance()`) iterates the per-rq leaf `cfs_rq` list and encounters a `cfs_rq` that has already been freed by `free_fair_sched_group()`. This typically crashes with a NULL pointer dereference or general protection fault. On systems with `SLAB_FREELIST_HARDENED`, the mangled freelist pointer overlapping with `struct sched_entity`'s `my_q` member causes a reliable `#GP` fault.

## Root Cause

The root cause is a race condition between two concurrent code paths that operate on the same `cfs_rq` objects without adequate synchronization.

**Path 1 — Cgroup teardown (CPU1):** When a cgroup is removed, the kernel calls `cpu_cgroup_css_released()` → `sched_offline_group()`. In the buggy code, `sched_offline_group()` first calls `unregister_fair_sched_group(tg)`, which iterates over all possible CPUs and for each one: removes the scheduling entity's load average via `remove_entity_load_avg(tg->se[cpu])`, then takes `rq->lock` and calls `list_del_leaf_cfs_rq(tg->cfs_rq[cpu])` to unlink the `cfs_rq` from the per-rq leaf list. After `unregister_fair_sched_group()` returns, `sched_offline_group()` takes `task_group_lock` and calls `list_del_rcu(&tg->list)` and `list_del_rcu(&tg->siblings)` to unlink the task group from the global task group hierarchy. Later, `sched_destroy_group()` schedules an RCU callback (`sched_free_group_rcu`) that calls `sched_free_group()` → `free_fair_sched_group()`, which `kfree()`'s all `cfs_rq` and `se` objects.

**Path 2 — CFS bandwidth period timer (CPU2):** The `sched_cfs_period_timer()` fires periodically and calls `do_sched_cfs_period_timer()` → `distribute_cfs_runtime()`. Inside an `rcu_read_lock()` critical section, `distribute_cfs_runtime()` iterates the throttled `cfs_rq` list and calls `unthrottle_cfs_rq()` for each. `unthrottle_cfs_rq()` calls `walk_tg_tree_from(tg, tg_nop, tg_unthrottle_up, ...)` which walks up the task group hierarchy using `list_for_each_entry_rcu(child, &parent->children, siblings)`. For each task group encountered, `tg_unthrottle_up()` retrieves `tg->cfs_rq[cpu_of(rq)]` and checks: `if (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running)` — if this condition is true, it calls `list_add_leaf_cfs_rq(cfs_rq)` to re-add the `cfs_rq` to the per-rq leaf list.

**The race interleaving:** The exact problematic interleaving documented in the commit message is:

1. CPU2 enters `distribute_cfs_runtime()` with `rcu_read_lock()` and begins walking the task group tree via `walk_tg_tree_from()`.
2. CPU2's `list_for_each_entry_rcu()` reads the dying task group's `siblings` entry from the parent's `children` list at point **(1)** — it now has a reference to the dying `tg`.
3. CPU1 calls `list_del_rcu(&tg->list)` and `list_del_rcu(&tg->siblings)` at point **(2)**, unlinking the task group. But this doesn't help because CPU2 already obtained its `next` pointer via RCU-protected traversal.
4. CPU1 proceeds to `unregister_fair_sched_group()` and calls `list_del_leaf_cfs_rq(tg->cfs_rq[cpu])` for each CPU, removing the `cfs_rq`'s from the leaf lists.
5. Meanwhile, CPU2's `tg_unthrottle_up()` checks `!cfs_rq_is_decayed(cfs_rq)` for the dying `tg`. Because the `cfs_rq` has residual undecayed load (even though there are no running tasks), this condition is true, and CPU2 calls `list_add_leaf_cfs_rq(cfs_rq)` at point **(3)** — re-adding the `cfs_rq` that CPU1 just unlinked.
6. CPU2 eventually exits the `rcu_read_unlock()` at point **(4)**.
7. The RCU grace period eventually expires, triggering `sched_free_group_rcu` → `sched_free_group` → `free_fair_sched_group` → `kfree(tg->cfs_rq[i])`. But the `cfs_rq` is still linked on the per-rq leaf list!
8. When `update_blocked_averages()` next iterates the leaf list, it dereferences the freed `cfs_rq`, causing a use-after-free.

The critical ordering error is that in the buggy code, `unregister_fair_sched_group()` (which unlinks `cfs_rq`'s) runs BEFORE the task group is unlinked from the hierarchy. This means concurrent RCU walkers can still find the task group and re-add its `cfs_rq`'s after they've been unlinked. Even if the order were reversed (unlink tg first, then unregister), a concurrent RCU reader that already obtained a reference to the dying tg would still be able to re-add its `cfs_rq`'s.

## Consequence

The primary consequence is a **kernel crash due to use-after-free** in `update_blocked_averages()`. The crash manifests as either a NULL pointer dereference or a general protection fault (`#GP`), depending on how the SLUB allocator reuses the freed memory.

The crash occurs in the scheduler softirq context (`run_rebalance_domains` → `__softirqentry_text_start`), which runs as part of `irq_exit()` after a timer interrupt. The affected CPU is typically in the idle loop (`cpuidle_enter_state`). The stack trace from Michal Koutný's report shows:

```
[exception RIP: update_blocked_averages+685]
#19 run_rebalance_domains
#20 __softirqentry_text_start
#21 irq_exit
#22 smp_apic_timer_interrupt
#23 apic_timer_interrupt
--- <IRQ stack> ---
#24 apic_timer_interrupt
[exception RIP: cpuidle_enter_state+171]
```

Because `update_blocked_averages()` holds the `rq->lock` when it crashes, all other CPUs that attempt to acquire the same `rq->lock` (for wakeups, migrations, etc.) will deadlock. This causes an NMI watchdog hard lockup on other CPUs:

```
[ 8995.095798] BUG: kernel NULL pointer dereference, address: 0000000000000080
[ 9016.281685] NMI watchdog: Watchdog detected hard LOCKUP on cpu 21
```

On systems with `SLAB_FREELIST_HARDENED` enabled, the bug is more reliably detected because the SLUB freelist pointer mangling causes the `my_q` pointer in the freed `struct sched_entity` to contain a garbled value, leading to a deterministic `#GP` fault instead of silent corruption.

Kevin Tanguy's production report and Michal Koutný's analysis both confirm that this bug triggers in real-world container/Kubernetes environments where cgroups with CPU bandwidth limits are frequently created and destroyed. Michal's reproducer script (`run2.sh`) could trigger the crash within minutes on a 5.15 kernel by rapidly cycling cgroup creation, task execution, and cgroup removal with a carefully tuned sleep to hit the 15ms race window.

## Fix Summary

The fix restructures the cgroup teardown sequence to ensure proper ordering with respect to RCU grace periods, preventing the dying task group from being visible to concurrent RCU walkers when `unregister_fair_sched_group()` runs.

**Key change 1 — Rename and reorder `sched_offline_group()` to `sched_release_group()`:** The old `sched_offline_group()` called `unregister_fair_sched_group()` first, then unlinked the task group. The new `sched_release_group()` ONLY unlinks the task group from the hierarchy (`list_del_rcu(&tg->list)` and `list_del_rcu(&tg->siblings)`). It no longer calls `unregister_fair_sched_group()` at all. This is called from `cpu_cgroup_css_released()`.

**Key change 2 — Defer `unregister_fair_sched_group()` to after an RCU grace period:** The new `sched_destroy_group()` (called from `cpu_cgroup_css_free()`) now calls `call_rcu(&tg->rcu, sched_unregister_group_rcu)`. The RCU callback `sched_unregister_group_rcu()` calls `sched_unregister_group()`, which calls `unregister_fair_sched_group(tg)` and `unregister_rt_sched_group(tg)`. Since `sched_release_group()` has already unlinked the task group before this RCU grace period, all concurrent RCU walkers that might have seen the dying task group have finished their `rcu_read_unlock()` by the time `unregister_fair_sched_group()` runs. No new walker can find the dying task group because it's no longer in the hierarchy. Therefore, `cfs_rq`'s cannot be re-added after they've been unlinked.

**Key change 3 — Second RCU grace period before freeing:** After `unregister_fair_sched_group()` runs (inside `sched_unregister_group()`), a second `call_rcu()` is used before calling `sched_free_group()`. This ensures that concurrent callers of `print_cfs_stats()` (which walks the leaf `cfs_rq` list under `rcu_read_lock()`) have finished reading the `cfs_rq` data before it is freed.

**Key change 4 — Move `destroy_cfs_bandwidth()` from `free_fair_sched_group()` to `unregister_fair_sched_group()`:** The CFS bandwidth timer must be stopped before freeing, and this is now done during unregistration rather than during freeing, as `unregister_fair_sched_group()` is the natural teardown point. Similarly, `destroy_rt_bandwidth()` is split into a new `unregister_rt_sched_group()` function.

The net effect is a two-phase teardown with two RCU grace period barriers: (1) unlink tg from hierarchy → RCU GP → (2) unregister cfs_rq's → RCU GP → (3) free memory. This eliminates both the original race and the secondary RCU reader race.

## Triggering Conditions

The bug requires all of the following conditions to be met simultaneously:

1. **CONFIG_FAIR_GROUP_SCHED and CONFIG_CFS_BANDWIDTH must be enabled.** These are standard in distribution kernels. CFS bandwidth throttling is the mechanism that causes the period timer to fire and walk the task group hierarchy.

2. **At least 2 CPUs.** The race is between two CPU-local paths: the cgroup teardown on one CPU and the CFS period timer interrupt on another CPU. On a single CPU system, the timer interrupt can still race with the teardown, but the probability is lower.

3. **CFS bandwidth quota must be configured on the cgroup being destroyed.** The cgroup must have `cpu.cfs_quota_us` (cgroupv1) or `cpu.max` (cgroupv2) set to a value that causes throttling. The reproducer uses a very small quota (2500µs) to ensure throttling occurs quickly.

4. **A task must have recently run in the cgroup** to generate load that takes time to decay. The `cfs_rq` must have `!cfs_rq_is_decayed(cfs_rq)` — i.e., residual PELT load averages. The task should have run long enough to accumulate measurable load (at least ~1 second).

5. **The cgroup must be destroyed shortly after the task exits**, while the `cfs_rq` still has undecayed load. The PELT decay halflife is ~32ms, so there is a window of roughly 100-200ms after task exit where the `cfs_rq` has non-zero residual load.

6. **The CFS period timer must fire during the teardown window.** The default CFS bandwidth period is 100ms (`cpu.cfs_period_us = 100000`). The teardown window (between `unregister_fair_sched_group()` and `free_fair_sched_group()`) was measured by Michal Koutný to be approximately 15ms. The probability of the timer firing during this window is roughly `15ms / 100ms = 15%` per cgroup destruction. With rapid cycling of cgroup create/destroy, the bug triggers within minutes.

7. **The kernel version must be between v5.13 (where `a7b359fc6a37` was merged) and v5.16-rc1 (where the fix was merged).** The bug specifically exists in v5.15 and earlier v5.14/v5.13 kernels that include the `a7b359fc6a37` commit.

8. **SLAB allocator activity helps reveal the bug.** The reproducer uses `setfattr` in a loop to stress the SLUB allocator's `kmalloc-512` cache, causing the freed `cfs_rq` memory to be quickly overwritten with other data. This makes the UAF crash faster and more deterministic rather than silently corrupting memory.

## Reproduce Strategy (kSTEP)

The reproduction requires the following kSTEP setup and sequence. Note that one minor kSTEP extension is needed: a `kstep_cgroup_destroy(name)` function (or equivalent `kstep_rmdir()`) to remove cgroups from kernel space. This is a trivial addition mirroring the existing `kstep_mkdir()` — it would call `vfs_rmdir()` from kernel space. The bandwidth quota can be set using the existing `kstep_cgroup_write()` API.

**Step 1 — Topology and configuration:**
Configure QEMU with at least 2 CPUs. No special topology is needed. The driver runs on CPU 0; tasks and timer interrupts fire on CPU 1+.

**Step 2 — Create a parent cgroup with bandwidth:**
```c
kstep_cgroup_create("bw_parent");
// cgroupv2: "quota period" format; 50000us quota, 100000us period
kstep_cgroup_write("bw_parent", "cpu.max", "50000 100000");
```
This ensures the CFS bandwidth timer is active for the parent group's bandwidth pool.

**Step 3 — Iteration loop to hit the race:**
The bug is probabilistic — the timer must fire during the ~15ms teardown window. The driver should loop multiple iterations (50-200), each iteration creating a child cgroup, running a task, destroying the cgroup, and checking for corruption.

Each iteration:

a. **Create a child cgroup with very small quota:**
```c
kstep_cgroup_create("bw_parent/child_N");
kstep_cgroup_write("bw_parent/child_N", "cpu.max", "2500 100000");
```
The 2500µs quota ensures quick throttling.

b. **Create a task and assign it to the child cgroup:**
```c
struct task_struct *p = kstep_task_create();
kstep_task_pin(p, 1, 2);  // pin to CPU 1
kstep_cgroup_add_task("bw_parent/child_N", p->pid);
kstep_task_wakeup(p);
```

c. **Run the task to consume quota and generate load:**
Advance time with `kstep_tick_repeat(N)` or `kstep_sleep()` for approximately 1 second to let the task accumulate PELT load and get throttled. The CFS period timer should fire during this time, distributing runtime and unthrottling as needed.

d. **Stop the task while leaving residual load:**
```c
kstep_task_pause(p);  // or kstep_task_block(p)
```
At this point, the `cfs_rq` for `bw_parent/child_N` on CPU 1 has `nr_running == 0` but still has undecayed PELT load (`!cfs_rq_is_decayed(cfs_rq)` is true).

e. **Destroy the child cgroup:**
```c
kstep_cgroup_destroy("bw_parent/child_N");  // needs kstep_rmdir addition
```
This initiates the asynchronous teardown: `css_released` → `sched_offline_group()` → `unregister_fair_sched_group()`, then after an RCU grace period, `css_free` → `free_fair_sched_group()`.

f. **Advance time to process the teardown and trigger the timer race:**
```c
kstep_tick_repeat(50);  // advance ticks to process workqueues and timers
kstep_sleep();          // allow RCU callbacks and workqueues to execute
```
The CFS period timer should fire during this window. On the buggy kernel, `tg_unthrottle_up()` may re-add the child's `cfs_rq` to the leaf list after `unregister_fair_sched_group()` has already unlinked it.

g. **Advance more time to trigger `update_blocked_averages()`:**
```c
kstep_tick_repeat(100);  // trigger scheduler balancing/blocked averages update
```
On the buggy kernel, if the race was hit, `update_blocked_averages()` will access the freed `cfs_rq`, causing a crash (NULL pointer deref or #GP).

**Step 4 — Detection:**
There are several ways to detect the bug:

- **Crash detection:** The most direct signal. If the QEMU VM crashes or panics during `update_blocked_averages()`, the bug is reproduced. Check `dmesg` / `data/logs/latest.log` for "BUG: kernel NULL pointer dereference" or "general protection fault".

- **Pre-free checking (more controlled):** Before the cgroup is fully freed, use `KSYM_IMPORT` to access internal scheduler state and check if any `cfs_rq` for the dying task group is still on a leaf list (`cfs_rq->on_list == 1`). This requires hooking into the teardown sequence, potentially via a callback or by manually accessing the `cfs_rq` between teardown phases. Specifically, after `unregister_fair_sched_group()` completes, check:
```c
struct cfs_rq *cfs_rq = tg->cfs_rq[1];  // CPU 1's cfs_rq
if (cfs_rq->on_list) {
    kstep_fail("cfs_rq still on leaf list after unregister!");
}
```

- **SLUB poison detection:** If `CONFIG_SLUB_DEBUG` is enabled, the freed memory will be poisoned, making the UAF crash more deterministic and the cause more obvious in logs.

**Step 5 — Verification on fixed kernel:**
On the fixed kernel (v5.16-rc1+), the same sequence should complete without any crash. The `sched_release_group()` unlinks the task group first, then after an RCU grace period, `sched_unregister_group()` safely unlinks the `cfs_rq`'s. The `cfs_rq`'s cannot be re-added because no RCU walker can find the dying task group anymore.

**Required kSTEP changes:**
1. **Add `kstep_rmdir(path)` or `kstep_cgroup_destroy(name)`:** This is the mirror of `kstep_mkdir()`. Implementation would use `kern_path()` to look up the path, then `vfs_rmdir()` to remove the directory. This is a ~15 line function. The cgroup must be empty (no tasks, no child cgroups) for rmdir to succeed.

2. **Ensure RCU grace periods advance during `kstep_sleep()`/`kstep_tick()`:** The cgroup teardown uses `call_rcu()` callbacks and workqueues. kSTEP's time advancement functions must allow these to complete. This may already work if `kstep_tick()` calls `rcu_check_callbacks()` or equivalent, and if the workqueue kthreads are allowed to run.

**Expected behavior:**
- **Buggy kernel (v5.15):** After several iterations, `update_blocked_averages()` should crash when dereferencing a freed `cfs_rq`, or the pre-free check should detect `on_list == 1` on a `cfs_rq` that was supposed to be unlinked. The driver should call `kstep_fail()`.
- **Fixed kernel (v5.16-rc1):** All iterations complete without crash or `on_list` anomaly. The driver should call `kstep_pass()`.

**Note on reliability:** Because this is a race condition, not every iteration will trigger the bug. The probability per iteration depends on whether the CFS period timer fires during the ~15ms teardown window. With a 100ms period, roughly 15% of iterations should hit the window. Over 50 iterations, the cumulative probability of at least one hit is approximately `1 - (0.85)^50 ≈ 99.97%`. If the timer period is reduced or the teardown window is widened by adding SLUB stress, reliability increases further.
