# Bandwidth: Missing Ancestor cfs_rq on Leaf List During Unthrottle

**Commit:** `2630cde26711dab0d0b56a8be1616475be646d13`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.15-rc4
**Buggy since:** v5.13-rc7 (commit `a7b359fc6a37` — "sched/fair: Correctly insert cfs_rq's to list on unthrottle")

## Bug Description

The Linux CFS scheduler maintains a "leaf list" (`rq->leaf_cfs_rq_list`) of `cfs_rq` structures that need their PELT (Per-Entity Load Tracking) averages updated during `update_blocked_averages()`. This list must maintain a strict parent-child ordering invariant: every child `cfs_rq` on the list must have all its ancestors also on the list, and children must appear before their parents. The `rq->tmp_alone_branch` pointer tracks an incomplete branch (a chain of `cfs_rq`s that have been added to the list but whose parent has not yet been added).

Commit `a7b359fc6a37` introduced logic to re-add `cfs_rq` nodes with undecayed load averages to the leaf list during unthrottle, even if they had no runnable tasks. This was necessary because during throttling, descendant `cfs_rq` nodes are removed from the leaf list, and when unthrottled, their load averages may not have fully decayed yet—they still need to be on the leaf list to be properly decayed during `update_blocked_averages()`. However, this change introduced a subtle bug in the `unthrottle_cfs_rq()` function.

When a `cfs_rq` is unthrottled but has no runnable tasks (`cfs_rq->load.weight == 0`), the function returns early without walking up the hierarchy to add ancestor `cfs_rq` nodes. If the `tg_unthrottle_up()` callback (called via `walk_tg_tree_from()`) had already placed the unthrottled `cfs_rq` on the leaf list (because it was not yet decayed), returning early means the ancestor `cfs_rq` nodes are never added. This breaks the `tmp_alone_branch` invariant, leading to a disconnected branch on the leaf list.

A partial fix was attempted in commit `fdaba61ef8a2` ("sched/fair: Ensure that the CFS parent is added after unthrottling"), which added `child_cfs_rq_on_list()` checks to `cfs_rq_is_decayed()`. However, this did not fully resolve the problem because the early return in `unthrottle_cfs_rq()` bypasses the ancestor-adding loop (`unthrottle_throttle` label) entirely. The author of the fix (Michal Koutný) confirmed that the LTP test `cfs_bandwidth01` still failed approximately 1 in 1000 executions.

## Root Cause

The root cause is in the `unthrottle_cfs_rq()` function in `kernel/sched/fair.c`. Before the fix, the code at the early-bail-out point looked like this:

```c
/* update hierarchical throttle state */
walk_tg_tree_from(cfs_rq->tg, tg_nop, tg_unthrottle_up, (void *)rq);

if (!cfs_rq->load.weight)
    return;
```

The `walk_tg_tree_from()` call invokes `tg_unthrottle_up()` on each ancestor, which decrements the `throttle_count` and, crucially, calls `list_add_leaf_cfs_rq(cfs_rq)` if the `cfs_rq` is not fully decayed or has running entities:

```c
static int tg_unthrottle_up(struct task_group *tg, void *data) {
    ...
    cfs_rq->throttle_count--;
    if (!cfs_rq->throttle_count) {
        ...
        if (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running)
            list_add_leaf_cfs_rq(cfs_rq);
    }
    return 0;
}
```

The `walk_tg_tree_from()` walks the tree bottom-up (post-order), so descendant `cfs_rq` nodes are processed before ancestor `cfs_rq` nodes. When `tg_unthrottle_up()` adds a descendant `cfs_rq` to the leaf list via `list_add_leaf_cfs_rq()`, that function may set `rq->tmp_alone_branch` to point to the newly added node, indicating that its parent still needs to be added. However, `tg_unthrottle_up()` does NOT guarantee that all ancestors get added—it only adds a `cfs_rq` if it is not decayed or has running entities. An ancestor that is fully decayed with no running entities will NOT be added by `tg_unthrottle_up()`.

After `walk_tg_tree_from()` completes, the unthrottled `cfs_rq` itself might have `load.weight == 0` (no runnable tasks). In the buggy code, this causes an immediate `return`, skipping the `unthrottle_throttle` label and its critical ancestor-fixup loop:

```c
unthrottle_throttle:
    for_each_sched_entity(se) {
        struct cfs_rq *qcfs_rq = cfs_rq_of(se);
        if (list_add_leaf_cfs_rq(qcfs_rq))
            break;
    }
    assert_list_leaf_cfs_rq(rq);
```

This loop walks up from the current scheduling entity and adds each ancestor `cfs_rq` to the leaf list via `list_add_leaf_cfs_rq()`, stopping when the branch connects to an existing part of the tree (indicated by `list_add_leaf_cfs_rq()` returning `true`). Without this loop, `tmp_alone_branch` remains pointing to the disconnected descendant `cfs_rq`, violating the invariant `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list`.

The specific interleaving that triggers this bug requires:
1. A nested cgroup hierarchy (at least 3 levels deep, e.g., root → level2 → level3 → worker).
2. A task in a deep cgroup gets throttled (its `cfs_rq` and descendants are removed from the leaf list).
3. An intermediate `cfs_rq` (e.g., level3) decays completely while throttled (load, util, and runnable sums all reach zero).
4. The task's `cfs_rq` is unthrottled. During `tg_unthrottle_up()`, the task's own `cfs_rq` is added to the leaf list (because it still has undecayed load), but the intermediate fully-decayed ancestor is NOT added.
5. The unthrottled `cfs_rq` has `load.weight == 0` (no runnable tasks), so `unthrottle_cfs_rq()` returns early, skipping the ancestor-fixup loop.

## Consequence

The broken `tmp_alone_branch` invariant causes `assert_list_leaf_cfs_rq(rq)` to trigger a `SCHED_WARN_ON`, producing a kernel warning:

```
SCHED_WARN_ON(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list)
```

This warning was reported when running the LTP (Linux Test Project) test `cfs_bandwidth01`, which exercises CFS bandwidth throttling with nested cgroup hierarchies. The warning occurs approximately 1 in 1000 test executions, indicating a timing-dependent race.

Beyond the warning, the broken leaf list has more serious consequences for scheduling fairness. When `update_blocked_averages()` iterates the leaf list, it may miss `cfs_rq` nodes whose ancestors are not on the list. This means their PELT load averages are not properly decayed, leading to stale load values. Stale load averages can cause severe fairness issues—as documented in the original commit `a7b359fc6a37`, two equally weighted sibling control groups could see a load distribution of 99:1 instead of 50:50. The decayed load values affect load balancing decisions, CPU utilization calculations, and task placement across the system.

Additionally, the disconnected branch on the leaf list could cause iteration issues in `update_blocked_averages()` where the list traversal encounters inconsistent state, potentially leading to infinite loops or skipped updates.

## Fix Summary

The fix modifies the early-return path in `unthrottle_cfs_rq()` to check whether the unthrottled `cfs_rq` was placed on the leaf list (indicated by `cfs_rq->on_list` being set). If it was, instead of returning immediately, the code jumps to the `unthrottle_throttle` label to execute the ancestor-fixup loop:

```c
/* Nothing to run but something to decay (on_list)? Complete the branch */
if (!cfs_rq->load.weight) {
    if (cfs_rq->on_list)
        goto unthrottle_throttle;
    return;
}
```

The `unthrottle_throttle` label leads to the `for_each_sched_entity(se)` loop that walks up the hierarchy, calling `list_add_leaf_cfs_rq(qcfs_rq)` on each ancestor's `cfs_rq`. This loop adds any missing ancestors to the leaf list, connecting the branch to the rest of the tree. The loop breaks when `list_add_leaf_cfs_rq()` returns `true`, indicating the branch has been connected (either the parent was already on the list, or we reached the root).

This fix is correct and complete because: (1) it preserves the optimization of returning early when the `cfs_rq` was NOT added to the leaf list (`!cfs_rq->on_list`), since in that case no ancestor fixup is needed; (2) it reuses the existing ancestor-fixup loop rather than duplicating code, which was a design choice refined across versions v1→v3 of the patch; (3) the `se` variable is correctly set to `cfs_rq->tg->se[cpu_of(rq)]` at the top of `unthrottle_cfs_rq()`, so the `for_each_sched_entity(se)` loop at the `unthrottle_throttle` label will properly walk from the unthrottled group's scheduling entity up to the root.

## Triggering Conditions

The bug requires the following precise conditions:

**Kernel configuration:** `CONFIG_FAIR_GROUP_SCHED=y`, `CONFIG_CFS_BANDWIDTH=y`, and `CONFIG_SMP=y` (for PELT load tracking and leaf list management). These are all enabled by default in standard distro kernels.

**Cgroup hierarchy:** A nested cgroup hierarchy with at least 3 levels of depth is needed (root task group → intermediate group → leaf group containing the task). The intermediate group's `cfs_rq` must fully decay while throttled—this requires that the intermediate group has no other running tasks and enough time passes for PELT averages to reach zero.

**Bandwidth throttling:** CFS bandwidth control must be enabled on the cgroup (via `cpu.cfs_quota_us` and `cpu.cfs_period_us`). The quota must be low enough that tasks get throttled, and the period must allow for enough time during throttling for intermediate `cfs_rq` load averages to decay.

**Task behavior:** A task in the leaf cgroup must run, get throttled (consuming its bandwidth quota), and then have its `cfs_rq` unthrottled when bandwidth is replenished. At the time of unthrottle, the `cfs_rq` must have `load.weight == 0` (no runnable tasks—the task may have blocked or exited) but still have undecayed PELT averages (`cfs_rq_is_decayed()` returns false), which causes `tg_unthrottle_up()` to add it to the leaf list.

**Timing:** The window is narrow—the bug only triggers when an intermediate ancestor's PELT averages have fully decayed (so `tg_unthrottle_up()` does NOT add it to the leaf list) but a descendant's averages have NOT fully decayed (so `tg_unthrottle_up()` DOES add it). This requires the intermediate group to have had no activity for long enough that all its PELT sums (`load_sum`, `util_sum`, `runnable_sum`) have decayed to zero—typically many milliseconds of inactivity. The ~1/1000 reproduction rate suggests this timing window is small but not vanishingly rare.

**Number of CPUs:** At least 1 CPU is sufficient, but the cgroup hierarchy must be present on the CPU where the unthrottle occurs.

## Reproduce Strategy (kSTEP)

The strategy leverages kSTEP's cgroup support and tick control to deterministically create the conditions for the bug. The key insight is to create a nested cgroup hierarchy, run a task to trigger bandwidth throttling, let intermediate cgroup load averages decay, and then observe the broken `tmp_alone_branch` invariant when the cgroup is unthrottled.

**Step 1: Topology and configuration.**
Configure QEMU with 2 CPUs (CPU 0 for the driver, CPU 1 for the test task). No special topology is needed beyond the default.

**Step 2: Create nested cgroup hierarchy.**
Using kSTEP's cgroup APIs, create a 3-level nested hierarchy:
```
kstep_cgroup_create("top");        // level 1
kstep_cgroup_create("top/mid");    // level 2 (intermediate)
kstep_cgroup_create("top/mid/leaf"); // level 3 (leaf)
```
Set bandwidth quota on the "top" cgroup to trigger throttling:
```
kstep_sysctl_write("/sys/fs/cgroup/cpu/top/cpu.cfs_quota_us", "%d", 1000);
kstep_sysctl_write("/sys/fs/cgroup/cpu/top/cpu.cfs_period_us", "%d", 100000);
```
This gives 1ms of runtime per 100ms period—very aggressive throttling.

**Step 3: Create and place a test task.**
```
struct task_struct *worker = kstep_task_create();
kstep_task_pin(worker, 1, 2);  // pin to CPU 1
kstep_cgroup_add_task("top/mid/leaf", worker->pid);
kstep_task_wakeup(worker);
```

**Step 4: Let the task run and get throttled.**
Advance ticks so the task consumes its bandwidth quota and gets throttled:
```
kstep_tick_repeat(100);  // enough ticks for the task to consume its 1ms quota
```
At this point, the "top" cgroup's `cfs_rq` should be throttled, and all descendant `cfs_rq` nodes removed from the leaf list.

**Step 5: Block the task and let PELT averages decay.**
While throttled, block the task so that when unthrottled, `cfs_rq->load.weight` will be 0:
```
kstep_task_block(worker);
```
Now advance many ticks to let the intermediate "top/mid" cgroup's PELT averages decay to zero, while the leaf "top/mid/leaf" cgroup's averages may still be non-zero:
```
kstep_tick_repeat(2000);  // let intermediate cfs_rq fully decay
```

**Step 6: Trigger unthrottle and observe.**
The bandwidth timer will eventually replenish quota and unthrottle the cgroup. Use ticks to advance past the period boundary. Before and after the unthrottle, use `KSYM_IMPORT` to access internal scheduler state:
```
KSYM_IMPORT(cpu_rq);
struct rq *rq1 = cpu_rq(1);
```
Check `rq1->tmp_alone_branch` against `&rq1->leaf_cfs_rq_list`. On the buggy kernel, after unthrottle, `tmp_alone_branch != &leaf_cfs_rq_list`, indicating the broken invariant. On the fixed kernel, they should be equal.

**Step 7: Pass/fail criteria.**
```
if (rq1->tmp_alone_branch == &rq1->leaf_cfs_rq_list)
    kstep_pass("tmp_alone_branch invariant maintained after unthrottle");
else
    kstep_fail("tmp_alone_branch invariant broken: branch not connected");
```

Alternatively, check whether the `cfs_rq` for the "mid" group has `on_list == 1` when the leaf cfs_rq has `on_list == 1`. On the buggy kernel, mid's `cfs_rq` will have `on_list == 0` while leaf's has `on_list == 1`, showing the missing ancestor.

**Step 8: Callbacks.**
Use `on_tick_end` or `on_sched_softirq_end` to inspect the `tmp_alone_branch` state after each tick, catching the exact moment the invariant breaks. This provides a more reliable detection mechanism than checking at a single point.

**Additional kSTEP considerations:**
- The driver needs to import `cpu_rq` and access internal `cfs_rq` structures via `internal.h` to read `tmp_alone_branch` and `on_list` fields.
- The cgroup bandwidth timer is driven by `hrtimer`, which fires based on the virtual clock. The `kstep_tick()` advances should trigger timer expiry and therefore unthrottle events.
- The `SCHED_WARN_ON` in `assert_list_leaf_cfs_rq()` will also fire on the buggy kernel, visible in `dmesg`/log output. This provides an additional signal for bug detection.
- Since the bug depends on PELT decay timing, the number of ticks needed between throttle and unthrottle may need tuning. Start with 2000+ ticks (at default 4ms interval = 8 seconds) to ensure full decay of the intermediate `cfs_rq`.

**Expected behavior:**
- **Buggy kernel:** After unthrottle with zero-weight `cfs_rq`, `tmp_alone_branch` points to a disconnected `cfs_rq` instead of `&rq->leaf_cfs_rq_list`. The `SCHED_WARN_ON` fires. The intermediate cgroup's `cfs_rq` is missing from the leaf list.
- **Fixed kernel:** After unthrottle, the code detects `cfs_rq->on_list` and jumps to `unthrottle_throttle`, which adds all ancestors. `tmp_alone_branch` equals `&rq->leaf_cfs_rq_list`. No warning fires.
