# Bandwidth: Incomplete Leaf List Maintenance in unthrottle_cfs_rq()

**Commit:** `39f23ce07b9355d05a64ae303ce20d1c4b92b957`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.7-rc7
**Buggy since:** v5.7-rc1 (commit `fe61468b2cbc` — "sched/fair: Fix enqueue_task_fair warning")

## Bug Description

The Linux CFS scheduler maintains a per-runqueue "leaf list" (`rq->leaf_cfs_rq_list`) that tracks which `cfs_rq` structures need PELT load-tracking updates. This list must maintain a strict ordering invariant: children must appear before their parents, tracked through the `rq->tmp_alone_branch` pointer. When CFS bandwidth control is enabled and a cgroup hierarchy is throttled and then unthrottled, the `unthrottle_cfs_rq()` function must re-add removed `cfs_rq` nodes to the leaf list while preserving this invariant.

Commit `fe61468b2cbc` ("sched/fair: Fix enqueue_task_fair warning") recognized that the `cfs_rq_throttled()` break in `unthrottle_cfs_rq()` could leave the leaf list in an incomplete state, and added a fixup loop at the end to re-add missing `cfs_rq` nodes. However, this fix was structurally incomplete: the function's single combined loop for both enqueuing new entities and updating already-enqueued entities did not handle intermediate `cfs_rq` nodes in a throttled hierarchy properly, and the fixup loop itself was suboptimal.

The bug was reported by Tao Zhou, who noticed that `unthrottle_cfs_rq()` did not follow the same structural pattern as `enqueue_task_fair()`, which had already received a similar fix. Phil Auld independently identified and submitted a related fix ("sched/fair: Fix enqueue_task_fair warning some more") where the `se` pointer used for the leaf list fixup could point to the wrong entity after the main loop modified it. Vincent Guittot's fix (this commit) addresses both `unthrottle_cfs_rq()` issues by restructuring it to match `enqueue_task_fair()`'s two-loop pattern.

## Root Cause

The root cause lies in the structural mismatch between `unthrottle_cfs_rq()` and `enqueue_task_fair()`. Before this fix, `unthrottle_cfs_rq()` used a single `for_each_sched_entity(se)` loop with an `enqueue` flag to switch behavior:

```c
int enqueue = 1;
for_each_sched_entity(se) {
    if (se->on_rq)
        enqueue = 0;
    cfs_rq = cfs_rq_of(se);
    if (enqueue) {
        enqueue_entity(cfs_rq, se, ENQUEUE_WAKEUP);
    } else {
        update_load_avg(cfs_rq, se, 0);
        se_update_runnable(se);
    }
    cfs_rq->h_nr_running += task_delta;
    cfs_rq->idle_h_nr_running += idle_task_delta;
    if (cfs_rq_throttled(cfs_rq))
        break;
}
```

There are three specific problems with this approach:

**Problem 1: Missing `throttled_hierarchy()` check.** In `enqueue_task_fair()`, the second loop (processing already-on-rq entities) includes a `throttled_hierarchy(cfs_rq)` check that calls `list_add_leaf_cfs_rq(cfs_rq)` to re-add `cfs_rq` nodes removed from the leaf list when a parent was throttled. The buggy `unthrottle_cfs_rq()` lacks this check entirely. When a `cfs_rq` is in a throttled hierarchy (i.e., its `throttle_count > 0` because an ancestor is throttled) but is itself not directly throttled, it may have been removed from the leaf list. Without the `throttled_hierarchy()` check and subsequent `list_add_leaf_cfs_rq()` call, this `cfs_rq` is never re-added, breaking the leaf list invariant.

**Problem 2: Wrong `update_load_avg` flags.** When `enqueue` is 0, the buggy code calls `update_load_avg(cfs_rq, se, 0)` with flags set to `0`. The correct flags should be `UPDATE_TG`, which triggers `update_tg_load_avg()` to propagate the task group's load to its parent. Without this flag, the hierarchical load tracking is not properly updated during unthrottle, which can lead to stale `tg_load_avg_contrib` values and consequently incorrect weight calculations for the task group.

**Problem 3: Suboptimal leaf list fixup.** The fixup loop at the end unconditionally calls `list_add_leaf_cfs_rq(cfs_rq)` without checking its return value. The function `list_add_leaf_cfs_rq()` returns `true` when the `cfs_rq` is successfully connected to the tree (either its parent is already on the list or it's the root `cfs_rq`), indicating that `rq->tmp_alone_branch` has been reset to `&rq->leaf_cfs_rq_list`. By not breaking early on `true`, the fixup loop does unnecessary work and, more critically, fails to correctly handle the case where the branch is already connected, potentially leaving `tmp_alone_branch` in an inconsistent state.

## Consequence

The primary observable consequence is a `SCHED_WARN_ON` assertion failure in `assert_list_leaf_cfs_rq(rq)`, which checks that `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list`. When this assertion fails, the kernel emits a warning with a stack trace originating from `unthrottle_cfs_rq()`. While this is a warning and not a crash, it indicates that the leaf list is in an inconsistent state.

A corrupted leaf list has cascading effects on the scheduler. The `for_each_leaf_cfs_rq_safe()` macro iterates over this list during `update_blocked_averages()`, which is called during load balancing. If `cfs_rq` nodes are missing from the leaf list, their PELT load averages will not be properly decayed, leading to stale `load_avg` and `util_avg` values. This causes load balancing to make incorrect decisions about CPU load, potentially leading to task placement problems, CPU underutilization, or unnecessary task migrations.

Additionally, the incorrect `update_load_avg` flags (missing `UPDATE_TG`) mean that task group shares are not properly recalculated during unthrottle operations. This can result in unfair bandwidth distribution among competing cgroups—a cgroup that has just been unthrottled may not receive its proportional CPU share because the parent's weight is based on stale data.

Guilherme G. Piccoli from Canonical reported encountering this same class of issues on production kernels (4.15.x and 5.4.x), confirming that the bug had real-world impact in containerized environments using CFS bandwidth control.

## Fix Summary

The fix restructures `unthrottle_cfs_rq()` to follow the exact same two-loop pattern as `enqueue_task_fair()`, addressing all three root-cause problems simultaneously.

**First loop (enqueue path):** The first `for_each_sched_entity(se)` loop now `break`s immediately when encountering `se->on_rq` (instead of setting `enqueue = 0` and continuing). Within this loop, each entity is enqueued with `enqueue_entity(cfs_rq, se, ENQUEUE_WAKEUP)`, and `h_nr_running` / `idle_h_nr_running` are updated. If a throttled `cfs_rq` is encountered, the loop jumps to `unthrottle_throttle` to perform leaf list fixup.

**Second loop (update path):** A separate `for_each_sched_entity(se)` loop handles entities that are already on the runqueue. This loop calls `update_load_avg(cfs_rq, se, UPDATE_TG)` (with the correct `UPDATE_TG` flag) and `se_update_runnable(se)`. Crucially, it includes the `throttled_hierarchy(cfs_rq)` check that calls `list_add_leaf_cfs_rq(cfs_rq)` to re-add `cfs_rq` nodes removed from the leaf list when a parent was throttled. If a directly throttled `cfs_rq` is encountered, it jumps to `unthrottle_throttle`. When this loop completes naturally (reaching the root), `add_nr_running(rq, task_delta)` is called unconditionally (the `if (!se)` check is no longer needed since reaching the end of the second loop guarantees `se == NULL`).

**Fixup loop (leaf list maintenance):** The leaf fixup loop at the `unthrottle_throttle` label now checks the return value of `list_add_leaf_cfs_rq(cfs_rq)` and breaks early when it returns `true`, indicating the branch is properly connected to the tree. This is both an optimization and a correctness fix, as it prevents unnecessary modifications to `tmp_alone_branch` after the list is already consistent.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

- **CFS bandwidth control enabled**: `CONFIG_CFS_BANDWIDTH=y` and `CONFIG_FAIR_GROUP_SCHED=y` must be set. The system must have cgroup v1 (with `cpu.cfs_quota_us`/`cpu.cfs_period_us`) or cgroup v2 (with `cpu.max`) configured with bandwidth limits.

- **Nested cgroup hierarchy with at least 3 levels**: A hierarchy of task groups is needed, e.g., `root -> A -> B -> C`, where bandwidth limits are set at an intermediate level (e.g., level A) such that throttling of A causes child `cfs_rq` nodes (B, C) to have their `throttle_count` incremented (making `throttled_hierarchy()` return true for them) and be removed from the leaf list.

- **Tasks running in the leaf cgroup**: Active tasks must be present in the deepest cgroup (e.g., C) so that `h_nr_running > 0` and the unthrottle path actually processes the full hierarchy.

- **Throttle/unthrottle cycle**: The intermediate cgroup (A) must be throttled (exhausting its quota) and then unthrottled (by the period timer replenishing quota). During unthrottle, the walk up the hierarchy must encounter entities that are already `on_rq` (because a higher-level entity was never dequeued) and `cfs_rq` nodes that are in a throttled hierarchy.

- **Concurrent throttling at different levels**: The most reliable trigger involves having bandwidth limits at multiple levels. For example, A has a tight quota and B has a separate quota. When A's quota expires, all descendants are throttled. When A is unthrottled, if B is simultaneously throttled (or has its `throttle_count > 0` from a still-throttled grandparent), the `throttled_hierarchy()` check becomes necessary.

The bug is deterministic given the right hierarchy configuration. It does not require a race condition per se, but the timing of throttle/unthrottle cycles relative to period timer expiration and task placement makes it probabilistic in practice. Production systems with multiple cgroups and tight bandwidth limits (e.g., container orchestration systems like Kubernetes with CPU limits) are most likely to encounter this.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**

   This bug exists exclusively in kernel versions v5.7-rc1 through v5.7-rc6. kSTEP supports Linux v5.15 and newer only. The buggy code was introduced by commit `fe61468b2cbc` (merged in v5.7-rc1) and fixed by commit `39f23ce07b93` (merged in v5.7-rc7). Since v5.7 is significantly older than v5.15, the kernel version falls outside kSTEP's supported range.

   The underlying code structure that this fix modifies (`unthrottle_cfs_rq()` with the single combined enqueue/update loop) no longer exists in v5.15+ kernels. By v5.15, the function already uses the two-loop pattern introduced by this fix, and subsequent fixes have further evolved the code.

2. **WHAT would need to be added to kSTEP to support this?**

   No framework changes to kSTEP are needed to reproduce this class of bug—kSTEP already has the necessary cgroup bandwidth APIs (`kstep_cgroup_write` with `cpu.max`), nested cgroup creation, task management, and internal state access. The only barrier is the kernel version requirement. If kSTEP were extended to support v5.7 kernels, the existing infrastructure would be sufficient to reproduce this bug.

   A reproduction driver would:
   - Create a nested cgroup hierarchy (root → A → B → C)
   - Set bandwidth limits on A (e.g., `kstep_cgroup_write("A", "cpu.max", "5000 100000")`)
   - Create tasks in C and wake them to consume A's quota
   - Tick until A is throttled, then wait for the period timer to unthrottle it
   - Check `rq->tmp_alone_branch != &rq->leaf_cfs_rq_list` after unthrottle to detect the assertion failure
   - Verify that `cfs_rq` nodes in the throttled hierarchy are properly present on the leaf list

3. **Kernel version too old**: The fix targets v5.7-rc7. kSTEP requires v5.15 or newer. This is the sole reason this bug cannot be reproduced with kSTEP.

4. **Alternative reproduction methods:**

   - **Direct kernel testing**: Build a v5.7-rc6 kernel with `CONFIG_CFS_BANDWIDTH=y`, `CONFIG_FAIR_GROUP_SCHED=y`, and `CONFIG_SCHED_DEBUG=y`. Create a nested cgroup hierarchy with bandwidth limits and run CPU-intensive tasks. The `SCHED_WARN_ON` in `assert_list_leaf_cfs_rq()` will fire when the leaf list becomes inconsistent. The LTP test `cfs_bandwidth01` exercises this code path.

   - **Targeted test script**: Create a cgroup v2 hierarchy:
     ```
     mkdir -p /sys/fs/cgroup/test/A/B/C
     echo "5000 100000" > /sys/fs/cgroup/test/A/cpu.max
     # Run CPU burners in C
     for i in $(seq 1 4); do
       taskset -c 1 stress-ng --cpu 1 --timeout 60 &
       echo $! > /sys/fs/cgroup/test/A/B/C/cgroup.procs
     done
     ```
     Monitor `dmesg` for the `SCHED_WARN_ON` from `assert_list_leaf_cfs_rq`.

   - **QEMU-based testing**: Boot a v5.7-rc6 kernel in QEMU with 2+ CPUs and run the above test. While this is outside kSTEP's framework, it uses the same QEMU-based approach that kSTEP employs.
