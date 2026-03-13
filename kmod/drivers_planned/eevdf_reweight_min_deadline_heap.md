# EEVDF: Reweight Entity Fails to Propagate min_deadline Heap Update

**Commit:** `8dafa9d0eb1a1550a0f4d462db9354161bc51e0c`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.6-rc6
**Buggy since:** v6.6-rc1 (introduced by commit `147f3efaa241` "sched/fair: Implement an EEVDF-like scheduling policy")

## Bug Description

The EEVDF (Earliest Eligible Virtual Deadline First) scheduler maintains a min-heap property on the `min_deadline` field of each `sched_entity` within the CFS red-black tree. This augmented data structure allows efficient O(log n) lookup of the entity with the earliest deadline among all eligible entities. The invariant is: `se->min_deadline = min(se->deadline, left->min_deadline, right->min_deadline)` for every node in the tree. When any node's deadline changes, this heap property must be propagated up to the root so that `pick_eevdf()` can correctly descend the tree to find the best scheduling candidate.

The `reweight_entity()` function is used to update the weight of a scheduling entity in-place — without performing a full dequeue/enqueue cycle — for performance reasons. This function is called by `update_cfs_group()` whenever the computed group share of a cgroup's scheduling entity changes (via `calc_group_shares()`), which happens frequently during `update_load_avg()` on the entity update path. When the entity is on the runqueue (`se->on_rq` is true) and is not the current task, `reweight_entity()` recalculates the entity's virtual deadline by scaling the remaining time-to-deadline by the ratio of old weight to new weight: `deadline = div_s64(deadline * old_weight, weight)`.

However, after modifying `se->deadline`, the function fails to propagate this change upward through the red-black tree's augmented `min_deadline` heap. This means that ancestor nodes in the tree may hold stale `min_deadline` values that no longer satisfy the heap invariant. Consequently, `pick_eevdf()` — which relies on the heap to efficiently find the earliest eligible deadline — may descend down the wrong branch of the tree and fail to find the correct scheduling entity.

This bug was reported by multiple developers (Marek Szyprowski, Biju Das, Mike Galbraith) who observed the `"EEVDF scheduling fail, picking leftmost"` error message, which is printed when `pick_eevdf()` returns NULL (indicating the heap search failed entirely) and the scheduler falls back to picking the leftmost entity in the tree. Mike Galbraith correlated the issue with cgroup scheduling and confirmed the min_deadline heap corruption through debug trace output.

## Root Cause

The root cause lies in the `reweight_entity()` function in `kernel/sched/fair.c`. When a group scheduling entity's weight is updated (because the cgroup's computed share has changed), and that entity is currently enqueued on the runqueue (`se->on_rq == true`) but is not the currently running entity (`cfs_rq->curr != se`), the function performs the following sequence:

1. Removes the entity from the `avg_vruntime` accounting: `avg_vruntime_sub(cfs_rq, se)`
2. Updates the load weight: `update_load_sub()`, `update_load_set()`
3. **Rescales the deadline**: `deadline = div_s64(deadline * old_weight, weight); se->deadline = se->vruntime + deadline;`
4. Re-adds the entity to `avg_vruntime` accounting: `avg_vruntime_add(cfs_rq, se)`

Step 3 directly modifies `se->deadline`, but the entity remains in the red-black tree throughout this operation (it is never dequeued and re-enqueued). The tree's augmented `min_deadline` heap is maintained by `RB_DECLARE_CALLBACKS(static, min_deadline_cb, ...)`, which generates a `min_deadline_cb_propagate()` function. This propagate function walks from a given node up to the root (or a stop node), recomputing `min_deadline` at each ancestor. It is normally called automatically during `rb_add_augmented_cached()` (enqueue) and `rb_erase_augmented_cached()` (dequeue), but since `reweight_entity()` deliberately avoids dequeue/enqueue for performance, neither of these tree operations occurs.

The missing call is `min_deadline_cb_propagate(&se->run_node, NULL)`, which would walk from the modified node up to the root, recomputing `min_deadline` at each ancestor. Without this call, the modified `se->deadline` value is not reflected in the `min_deadline` of its ancestors. If the new deadline is smaller than the old one, ancestors may fail to report it as the minimum. If the new deadline is larger than the old one, ancestors may still advertise the old (now stale) smaller value.

For example, consider a cgroup hierarchy where an autogroup entity has `min_deadline = -66302085` but the parent reports `min_deadline = -55861854`. This means the parent's `min_deadline` does not reflect its child's smaller deadline, violating the heap invariant. The debug trace from Mike Galbraith's report shows exactly this:

```
__print_se: ffff88845cf48080 w: 1024 ve: -58857638 vd: -55861854 vmd: -66302085
__print_se:   ffff88810d165800 w: 25 ve: -80323686 vd: -41496434 vmd: -66302085
validate_cfs_rq: min_deadline: -55861854
```

Here the root entity (w: 1024) has `vmd` (min_deadline) of `-66302085`, but the `cfs_rq->min_deadline` is `-55861854` (which equals the root's own `vd`). This is inconsistent — `cfs_rq`'s min_deadline should be `-66302085` (propagated from the child subtree), but the stale value remained from before the child's deadline was rescaled.

## Consequence

The primary observable consequence is the `"EEVDF scheduling fail, picking leftmost"` error message printed by `pick_eevdf()` when it fails to find any valid entity through the heap-guided search. When this happens, the scheduler falls back to `__pick_first_entity()`, which simply returns the leftmost (lowest vruntime) entity in the red-black tree. This fallback entity is not necessarily the one with the earliest eligible deadline, so scheduling decisions become suboptimal — the EEVDF policy is effectively violated.

In the worst case, the corrupted heap can cause `pick_eevdf()` to return `NULL` entirely (if the heap search follows a corrupted path that leads to no eligible entity). The fallback to the leftmost entity prevents a kernel crash, but it means that:

1. **Fairness violations**: Tasks that should be scheduled (because they have the earliest eligible deadline) are skipped in favor of the leftmost task, leading to scheduling unfairness and potential starvation of tasks in certain cgroups.
2. **Latency spikes**: Tasks with urgent deadlines may be delayed, causing unpredictable latency increases for latency-sensitive workloads.
3. **Performance degradation**: The EEVDF algorithm's O(log n) optimal scheduling guarantee degenerates — the scheduler may make consistently wrong choices about which task to run next when the heap is corrupted.

The bug is particularly impactful in cgroup-heavy environments (e.g., containers, systemd-managed services) where `update_cfs_group()` is called frequently, causing repeated reweight operations that each corrupt the heap further. The reporters observed this on real systems running typical desktop/server workloads (Marek on qemu/arm64 virt machine, Biju on Renesas platforms).

## Fix Summary

The fix is a single line addition in `reweight_entity()`. After the deadline is rescaled (`se->deadline = se->vruntime + deadline`), the fix calls:

```c
min_deadline_cb_propagate(&se->run_node, NULL);
```

This invokes the augmented tree propagation callback, which walks from the modified entity's node up to the root of the red-black tree. At each ancestor node, it recomputes `min_deadline = min(se->deadline, left->min_deadline, right->min_deadline)` via `min_deadline_update()`. The `NULL` stop parameter means propagation continues all the way to the root.

The `min_deadline_cb_propagate()` function is generated by the `RB_DECLARE_CALLBACKS` macro (in `include/linux/rbtree_augmented.h`). It iterates upward through `rb_parent()` pointers, calling `min_deadline_update()` at each node. If a node's `min_deadline` doesn't change (the `min_deadline_update()` function returns `true` when the value is unchanged), propagation stops early — this is the `break` in the generated `_propagate` function. This makes the fix efficient: in most cases, only a few ancestors need updating.

This fix is correct and complete because `reweight_entity()` is the only code path that modifies `se->deadline` while the entity remains in the tree without going through the full dequeue/enqueue cycle. All other deadline modifications either happen when the entity is not on the runqueue, or go through `__enqueue_entity()` which calls `rb_add_augmented_cached()` (which triggers automatic propagation). By adding the propagation call after the in-place deadline modification, the fix ensures the heap invariant is maintained in all cases.

## Triggering Conditions

The bug requires the following conditions:

- **Cgroup scheduling enabled** (`CONFIG_FAIR_GROUP_SCHED=y`): The bug is triggered through `update_cfs_group()` → `reweight_entity()`, which only exists when fair group scheduling is configured. This is the default on most distributions.

- **Multiple task groups with varying loads**: `calc_group_shares()` must compute a different share than the entity's current weight, causing `reweight_entity()` to actually modify the deadline. This happens naturally when tasks are added to or removed from cgroups, or when the load distribution among cgroups changes (e.g., tasks waking up or sleeping in different cgroups).

- **Entity must be on the runqueue but not current**: The deadline rescaling only happens in the `se->on_rq` branch of `reweight_entity()`, and `avg_vruntime_sub()` is only called when `cfs_rq->curr != se`. The entity whose deadline is corrupted must be an enqueued group scheduling entity (`se->my_q != NULL`) that is not the currently running entity on its parent's cfs_rq.

- **Multiple entities in the same cfs_rq**: The heap corruption is only observable when there are multiple entities in the same cfs_rq, since a single entity trivially satisfies the heap property. At least 2-3 entities (e.g., autogroup cgroups or explicit cgroups) are needed.

- **Enough scheduling activity to trigger load updates**: `update_cfs_group()` is called from `entity_tick()` → `update_curr()` → `update_load_avg()`, from `enqueue_entity()`, `dequeue_entity()`, and `put_prev_entity()`. Normal scheduling activity (ticks, wake-ups, sleeps) will trigger it.

The bug is highly reproducible in any system with active cgroup scheduling — the reporters saw it frequently during boot on diverse hardware. The corruption accumulates over time as more reweight operations occur without proper propagation.

## Reproduce Strategy (kSTEP)

The strategy is to create a cgroup hierarchy with multiple task groups, place tasks in them to generate ongoing load, and then observe the `min_deadline` heap invariant being violated after reweight operations.

### Step-by-step plan:

1. **Topology**: Use 2 CPUs (default QEMU configuration is sufficient). No special topology setup needed.

2. **Cgroup setup**: Create 2-3 cgroups with different weights to ensure `calc_group_shares()` produces varying share values:
   ```
   kstep_cgroup_create("grpA");
   kstep_cgroup_set_weight("grpA", 1024);   // default weight
   kstep_cgroup_create("grpB");
   kstep_cgroup_set_weight("grpB", 100);    // low weight
   kstep_cgroup_create("grpC");
   kstep_cgroup_set_weight("grpC", 5000);   // high weight
   ```

3. **Task creation**: Create 4-6 CFS tasks and distribute them across the cgroups to create load imbalance that triggers reweight:
   ```
   // 2 tasks in grpA
   p1 = kstep_task_create(); kstep_cgroup_add_task("grpA", p1->pid);
   p2 = kstep_task_create(); kstep_cgroup_add_task("grpA", p2->pid);
   // 1 task in grpB
   p3 = kstep_task_create(); kstep_cgroup_add_task("grpB", p3->pid);
   // 2 tasks in grpC
   p4 = kstep_task_create(); kstep_cgroup_add_task("grpC", p4->pid);
   p5 = kstep_task_create(); kstep_cgroup_add_task("grpC", p5->pid);
   ```
   Pin tasks across CPUs 1 and 2 (avoiding CPU 0) using `kstep_task_pin()`.

4. **Generate load variation**: Use `kstep_task_wakeup()` and `kstep_task_block()` to create dynamic load changes that trigger `update_cfs_group()`:
   - Wake all tasks
   - Advance several ticks with `kstep_tick_repeat(N)` to let the scheduler update load averages
   - Block some tasks to change the load balance between cgroups
   - Wake them again with different patterns

5. **Validation via `on_tick_end` callback**: After each tick, use `KSYM_IMPORT` to access internal scheduler structures and validate the min_deadline heap invariant. The validation should walk the RB tree of each CPU's cfs_rq and verify:
   ```c
   // For each node in the RB tree:
   // se->min_deadline == min(se->deadline,
   //                         left ? left->min_deadline : U64_MAX,
   //                         right ? right->min_deadline : U64_MAX)
   ```
   Use `KSYM_IMPORT` to import `cpu_rq` and access `cfs_rq->tasks_timeline` to walk the tree. For each `sched_entity`, check the heap invariant by reading `se->min_deadline`, `se->deadline`, and the children's `min_deadline` values.

6. **Pass/fail criteria**:
   - **On buggy kernel (pre-fix)**: After sufficient ticks with load variation causing reweight operations, the min_deadline heap invariant will be violated at some ancestor node. The driver should report `kstep_fail("min_deadline heap corrupted: se=%p min_deadline=%lld != computed_min=%lld", se, se->min_deadline, computed_min)`.
   - **On fixed kernel**: The heap invariant holds at all times. Report `kstep_pass("min_deadline heap invariant maintained after %d ticks", tick_count)`.

7. **Alternative detection**: As a secondary check, import the `pick_eevdf` function or monitor for the `"EEVDF scheduling fail, picking leftmost"` printk message in the kernel log. On the buggy kernel, this message should appear after enough reweight operations corrupt the heap. On the fixed kernel, it should never appear.

8. **Key internals to access**:
   - `cpu_rq(cpu)->cfs` — the root cfs_rq
   - `cfs_rq->tasks_timeline.rb_root.rb_node` — root of the RB tree
   - `__node_2_se(node)` or `rb_entry(node, struct sched_entity, run_node)` — convert RB node to sched_entity
   - `se->deadline`, `se->min_deadline`, `se->run_node.rb_left`, `se->run_node.rb_right` — the fields to validate
   - `se->on_rq`, `se->load.weight` — to confirm entities are enqueued and have varying weights

9. **Expected timing**: The heap corruption should manifest within 50-200 ticks of active scheduling with multiple cgroups, as `update_cfs_group()` is called on every `entity_tick()` → `update_curr()` → `update_load_avg()` path. Increasing the number of tasks and varying their wake/sleep patterns increases the probability that `calc_group_shares()` computes a different weight, triggering `reweight_entity()`.

10. **kSTEP compatibility note**: This bug is fully reproducible with kSTEP. The framework already provides cgroup creation (`kstep_cgroup_create`), weight setting (`kstep_cgroup_set_weight`), task management, and tick control. Internal access to `cpu_rq` and `cfs_rq` internals is available through `internal.h`. No kSTEP framework changes are needed.
