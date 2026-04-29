# Bandwidth: Unthrottle with Zero runtime_remaining Triggers WARN in tg_throttle_down

**Commit:** `956dfda6a70885f18c0f8236a461aa2bc4f556ad`
**Affected files:** `kernel/sched/core.c`, `kernel/sched/fair.c`
**Fixed in:** v6.18-rc5
**Buggy since:** v6.18-rc1 (introduced by commit `e1fad12dcb66` "sched/fair: Switch to task based throttle model")

## Bug Description

When CFS bandwidth control is configured in a nested cgroup hierarchy, a `WARN_ON_ONCE` fires in `tg_throttle_down()` due to a non-empty `throttled_limbo_list`. The bug occurs when a child task group (C) has its bandwidth quota set via `tg_set_cfs_bandwidth()`, which initializes its per-CPU `cfs_rq` with `runtime_remaining = 0` and `throttled = 0` (unthrottled). This creates an inconsistent state: the cfs_rq is considered unthrottled but has zero runtime, making it immediately eligible for throttling upon any enqueue.

The problematic scenario involves a hierarchical cgroup topology where both a parent group (A) and a descendant group (C) have quota settings. When A is throttled, tasks migrating to C's cfs_rq (e.g., due to `sched_move_task()` from task group changes) are placed directly onto C's `throttled_limbo_list` by `enqueue_throttled_task()`, because the hierarchy is throttled. Later, when A is unthrottled, `tg_unthrottle_up()` walks down the tree and re-enqueues these limbo tasks via `enqueue_task_fair()`.

The critical problem is that the first re-enqueue of a limbo task triggers `check_enqueue_throttle()` on C's cfs_rq. Because `runtime_remaining == 0`, and `account_cfs_rq_runtime()` cannot obtain more runtime from the bandwidth pool (it's depleted or the cfs_rq has none to begin with), `throttle_cfs_rq()` is called. This throttle call during the unthrottle walk invokes `tg_throttle_down()`, which asserts `WARN_ON_ONCE(!list_empty(&cfs_rq->throttled_limbo_list))`. Since there are still remaining tasks in the limbo list that haven't been processed yet by `tg_unthrottle_up()`, the warning fires. This represents a fundamental ordering violation: throttling should never occur on the unthrottle path.

## Root Cause

The root cause is in `tg_set_cfs_bandwidth()` in `kernel/sched/core.c`. When CFS bandwidth parameters are configured for a task group, the function iterates over all online CPUs and initializes each per-CPU `cfs_rq`:

```c
for_each_online_cpu(i) {
    struct cfs_rq *cfs_rq = tg->cfs_rq[i];
    struct rq *rq = cfs_rq->rq;

    guard(rq_lock_irq)(rq);
    cfs_rq->runtime_enabled = runtime_enabled;
    cfs_rq->runtime_remaining = 0;  // BUG: zero runtime but unthrottled

    if (cfs_rq->throttled)
        unthrottle_cfs_rq(cfs_rq);
}
```

The cfs_rq is left in a state where `runtime_enabled = 1`, `runtime_remaining = 0`, and `throttled = 0`. This is logically inconsistent: the cfs_rq claims to be unthrottled but has no runtime budget whatsoever.

The sequence of events that triggers the bug is:

1. **Setup**: Task groups A (parent), B (intermediate), and C (child of B, grandchild of A) exist. Both A and C have quota settings. A becomes throttled due to its bandwidth limit being exhausted.

2. **Limbo list population**: While A's hierarchy is throttled, tasks are moved to C's cfs_rq (e.g., via `sched_move_task()` during task group reassignment). `enqueue_throttled_task()` checks `throttled_hierarchy(cfs_rq)` and, finding C's cfs_rq is in a throttled hierarchy, places these tasks on `cfs_rq->throttled_limbo_list` instead of doing a full enqueue. Multiple tasks can accumulate on this list.

3. **C's quota re-set**: C's bandwidth quota is (re-)configured via `tg_set_cfs_bandwidth()`, setting `runtime_remaining = 0` while `throttled = 0`.

4. **A unthrottles**: When A's bandwidth period timer fires and distributes new runtime, `unthrottle_cfs_rq()` is called for A. This calls `walk_tg_tree_from(A->tg, tg_nop, tg_unthrottle_up, rq)`, which walks descendant cfs_rqs in bottom-up order.

5. **tg_unthrottle_up(C)**: For C's cfs_rq, the function decrements `throttle_count` and then iterates over `throttled_limbo_list`, calling `enqueue_task_fair(rq, p, ENQUEUE_WAKEUP)` for each limbo task.

6. **First enqueue triggers throttle**: The first `enqueue_task_fair()` call eventually reaches `check_enqueue_throttle()` for C's cfs_rq. Since `runtime_enabled == 1`, `cfs_rq->curr == NULL` (no current entity), and the cfs_rq is not yet throttled, it calls `account_cfs_rq_runtime(cfs_rq, 0)`. With `runtime_remaining == 0`, `__account_cfs_rq_runtime()` tries to obtain more runtime via `assign_cfs_rq_runtime()`, but the bandwidth pool is empty. So `runtime_remaining` stays at 0 (or goes negative), and `throttle_cfs_rq()` is called.

7. **WARN fires**: `throttle_cfs_rq()` calls `walk_tg_tree_from(C->tg, tg_throttle_down, tg_nop, rq)`. Inside `tg_throttle_down()`, the assertion `WARN_ON_ONCE(!list_empty(&cfs_rq->throttled_limbo_list))` fires because there are still remaining tasks in the limbo list that haven't been processed by `tg_unthrottle_up()` yet.

The guard in `unthrottle_cfs_rq()` that checks `cfs_rq->runtime_remaining <= 0` was specifically designed to prevent this scenario, but only for the case where `unthrottle_cfs_rq()` itself is called with zero runtime. The problem is that `tg_set_cfs_bandwidth()` creates a cfs_rq that is unthrottled with zero runtime, and the zero-runtime check only fires inside `unthrottle_cfs_rq()`, not during the `tg_unthrottle_up()` walk triggered by an ancestor's unthrottle.

## Consequence

The immediate consequence is a `WARN_ON_ONCE` kernel warning in `tg_throttle_down()`:

```
WARNING: CPU: X PID: Y at kernel/sched/fair.c:5977 tg_throttle_down+0xNN/0xNN
```

This warning indicates that `cfs_rq->throttled_limbo_list` is non-empty when a throttle operation begins, which violates the invariant that limbo lists must be empty before throttling. Beyond the warning, the cfs_rq ends up in a mixed state: it has been partially unthrottled (some limbo tasks were re-enqueued) but then re-throttled during the same unthrottle walk. This can lead to:

- **Task starvation**: Tasks that were on the limbo list but not yet re-enqueued when throttling occurred may be left in an inconsistent state. They are on the limbo list of a now-throttled cfs_rq, but the unthrottle walk has already passed this cfs_rq, so they may never be properly re-enqueued until the next unthrottle cycle.

- **Scheduler state corruption**: The interleaving of throttle and unthrottle operations on the same cfs_rq during a single walk can corrupt PELT (Per-Entity Load Tracking) accounting, throttle counters, and the throttled_list in `cfs_bandwidth`. The `throttled_clock`, `throttled_clock_pelt`, and related fields may be set to incorrect values since `tg_throttle_down()` assumes a clean transition.

- **Potential deadlocks or list corruption**: Since `throttle_cfs_rq()` modifies the `cfs_bandwidth.throttled_cfs_rq` list and the `tg_unthrottle_up()` walk modifies the limbo list simultaneously, there is a risk of list corruption if the same cfs_rq appears on multiple lists unexpectedly.

The bug was confirmed by multiple testers (K Prateek Nayak at AMD and Hao Jia at Lixiang) in stress testing scenarios involving nested cgroup hierarchies with bandwidth quotas.

## Fix Summary

The fix is a one-character change in `tg_set_cfs_bandwidth()` in `kernel/sched/core.c`:

```c
-    cfs_rq->runtime_remaining = 0;
+    cfs_rq->runtime_remaining = 1;
```

By granting 1 nanosecond of runtime to the cfs_rq when bandwidth is configured, the fix ensures that the cfs_rq starts in a consistent unthrottled state with a positive `runtime_remaining`. This tiny amount of runtime (1ns) is effectively zero for any practical scheduling purpose, but it prevents `check_enqueue_throttle()` from immediately triggering `throttle_cfs_rq()` when the first task is enqueued during an ancestor's unthrottle walk.

Specifically, when `check_enqueue_throttle()` calls `account_cfs_rq_runtime(cfs_rq, 0)`, the 1ns of `runtime_remaining` ensures the check `cfs_rq->runtime_remaining <= 0` in `check_enqueue_throttle()` evaluates to false, so `throttle_cfs_rq()` is never called on the unthrottle path. The 1ns will be consumed by the very first accounting operation, after which the cfs_rq will legitimately obtain runtime from the bandwidth pool or be properly throttled outside the unthrottle walk context.

The fix also updates the comment in `unthrottle_cfs_rq()` to reflect that `runtime_remaining == 0` is no longer a possible state when entering `unthrottle_cfs_rq()` from `tg_set_cfs_bandwidth()`. The comment now only mentions the `runtime_remaining < 0` case (which can happen when async unthrottling races with runtime consumption). A redundant second assignment of `se = cfs_rq->tg->se[cpu_of(rq)]` in `unthrottle_cfs_rq()` is also removed as cleanup.

## Triggering Conditions

The following precise conditions are needed to trigger the bug:

- **Kernel version**: v6.18-rc1 or later (after commit `e1fad12dcb66` which introduced the task-based throttle model with limbo lists), and before v6.18-rc5 (the fix).

- **CONFIG_CFS_BANDWIDTH=y**: CFS bandwidth control must be enabled in the kernel configuration. This is the default on most distributions.

- **CONFIG_CGROUP_SCHED=y**: Cgroup-based scheduling must be enabled.

- **Nested cgroup hierarchy with multiple bandwidth-limited groups**: At least two task groups in an ancestor-descendant relationship must have bandwidth quotas set. In the reported scenario, group A (ancestor) and group C (descendant through B) both have `cpu.max` (cgroup v2) or `cpu.cfs_quota_us`/`cpu.cfs_period_us` (cgroup v1) configured.

- **Parent group (A) must be throttled**: Group A must exhaust its bandwidth quota so that its hierarchy becomes throttled. This is achieved by running enough tasks to consume A's quota within a period.

- **Tasks must migrate to C's cfs_rq while A is throttled**: Multiple tasks need to be moved to C's cfs_rq while the hierarchy is throttled, causing them to land on C's `throttled_limbo_list`. This can happen via `sched_move_task()` (cgroup migration) or by creating/setting C's quota after tasks are already running.

- **C's bandwidth must be (re-)configured after limbo tasks accumulate**: `tg_set_cfs_bandwidth()` must run for C, resetting `runtime_remaining = 0` while limbo tasks are present. This is the step that creates the zero-runtime unthrottled state.

- **A must be unthrottled while C has zero runtime and non-empty limbo list**: When A's bandwidth period timer distributes new runtime and unthrottles A, the `tg_unthrottle_up()` walk will trigger the bug at C's cfs_rq.

- **Minimum 2 CPUs**: At least 2 CPUs are needed (one for the parent group's tasks to exhaust the quota, one for the child group's cfs_rq where the limbo list is populated). The reported scenario uses CPU 1 for C's limbo tasks.

- **Race timing**: The sequence of operations (A throttles → tasks migrate to C → C's quota is set → A unthrottles) must occur in this order. This is somewhat timing-dependent but reproducible under stress.

## Reproduce Strategy (kSTEP)

A kSTEP driver already exists at `kmod/drivers/throttled_limbo_list.c` and the commit is listed in `kmod/drivers_planned/list.txt`. Below is a detailed step-by-step strategy for reproduction:

**1. Topology and CPU setup:**
Configure QEMU with at least 3 CPUs. CPU 0 is reserved for the driver. CPU 1 will host C's cfs_rq where the bug manifests. CPU 2 hosts a helper task used to interact with C's bandwidth pool.

**2. Task creation:**
Create 3 main tasks (`tasks[0..2]`) and 1 helper task. All 3 main tasks are pinned to CPU 1 and assigned to cgroup `A/B/C`. The helper is pinned to CPU 2 and also assigned to `A/B/C`.

**3. Cgroup hierarchy setup:**
```
kstep_cgroup_create("A");
kstep_cgroup_create("A/B");
kstep_cgroup_create("A/B/C");
```
This creates the hierarchy: root → A → B → C.

**4. Bandwidth configuration:**
```
kstep_cgroup_write("A", "cpu.max", "5000 100000");      // A: 5ms quota, 100ms period
kstep_cgroup_write("A/B/C", "cpu.max", "100000 100000"); // C: 100ms quota, 100ms period (generous)
```
A has a tight 5ms/100ms quota. C initially has a generous quota.

**5. Wake tasks and exhaust A's quota:**
Wake all 3 tasks on CPU 1 and tick repeatedly. As they run, they consume A's 5ms quota. Once A is throttled, the hierarchy including C's cfs_rq becomes throttled. The tasks on CPU 1 are dequeued by the task-based throttle model and placed on C's `throttled_limbo_list` via `enqueue_throttled_task()`.

**6. Verify limbo list population:**
Use `KSYM_IMPORT` or direct internal access to read `cfs_rq_c->throttled_limbo_list` on CPU 1. Count entries and ensure at least 2 tasks are on the limbo list. The existing driver does this via the `count_limbo()` helper function.

**7. Re-set C's quota to trigger zero runtime_remaining:**
```
kstep_cgroup_write("A/B/C", "cpu.max", "1000 100000");  // C: 1ms quota, 100ms period
```
This calls `tg_set_cfs_bandwidth()` for C, which on the buggy kernel sets `cfs_rq->runtime_remaining = 0` for all C's per-CPU cfs_rqs. On the fixed kernel, it sets `runtime_remaining = 1`.

**8. Optionally drain C's runtime pool:**
Wake the helper on CPU 2 (also in group C) to consume whatever runtime C's bandwidth pool has, ensuring the pool cannot refill C's cfs_rq when `assign_cfs_rq_runtime()` is called.

**9. Trigger A's unthrottle:**
Tick repeatedly (e.g., 120 ticks at default 4ms interval = ~480ms, which spans A's 100ms period). When A's period timer fires and distributes new runtime, `unthrottle_cfs_rq()` is called for A. This triggers `tg_unthrottle_up()` to walk down the tree.

**10. Bug detection:**
On the **buggy kernel**: `tg_unthrottle_up(C)` re-enqueues the first limbo task → `check_enqueue_throttle()` finds `runtime_remaining == 0` → `throttle_cfs_rq()` is called → `tg_throttle_down()` fires `WARN_ON_ONCE(!list_empty(&cfs_rq->throttled_limbo_list))`. The kernel will emit a warning in `dmesg`/console output. The driver can detect this by checking for kernel warnings in the log output, or by directly reading `cfs_rq->throttled` state during the unthrottle walk (which should never be 1 during an unthrottle operation for a descendant cfs_rq).

On the **fixed kernel**: `tg_unthrottle_up(C)` re-enqueues limbo tasks successfully because `runtime_remaining == 1` prevents immediate throttling. No warning is emitted. The cfs_rq will eventually be properly throttled when its 1ns of runtime is consumed and regular throttle checking occurs outside the unthrottle walk.

**11. Pass/fail criteria:**
- Check `dmesg` or kernel log for `WARN_ON_ONCE` messages from `tg_throttle_down`.
- On buggy kernel: The warning fires → `kstep_fail("WARN triggered in tg_throttle_down")`.
- On fixed kernel: No warning → `kstep_pass("No WARN, unthrottle completed cleanly")`.
- Additionally, verify that after the unthrottle walk completes, C's limbo list is empty (all tasks were properly re-enqueued) and C's cfs_rq is in a consistent throttled/unthrottled state.

**12. kSTEP features used:**
- `kstep_cgroup_create()`, `kstep_cgroup_write()`, `kstep_cgroup_add_task()` for cgroup hierarchy and bandwidth configuration.
- `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_wakeup()` for task management.
- `kstep_tick()`, `kstep_tick_repeat()` for time progression.
- Direct access to `cfs_rq->throttled_limbo_list` via `internal.h` for limbo list inspection.
- `KSYM_IMPORT` may be needed for accessing internal symbols not directly available.

**13. No kSTEP extensions required:**
All necessary functionality (cgroup creation, bandwidth quota setting via `cpu.max`, task pinning, ticking) is already available in kSTEP. The `kstep_cgroup_write()` function can write arbitrary cgroup files including `cpu.max`. The existing driver at `kmod/drivers/throttled_limbo_list.c` demonstrates this is fully reproducible with current kSTEP capabilities.
