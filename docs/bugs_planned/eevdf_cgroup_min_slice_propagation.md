# EEVDF: Stale min_slice in Cgroup Hierarchy During Enqueue/Dequeue

**Commit:** `563bc2161b94571ea425bbe2cf69fd38e24cdedf`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.15-rc1
**Buggy since:** `aef6987d8954` ("sched/eevdf: Propagate min_slice up the cgroup hierarchy"), merged in v6.12-rc1

## Bug Description

The EEVDF scheduler propagates a `min_slice` value up the cgroup scheduling entity (se) hierarchy so that parent group entities receive timely service for their child entities with small time-slice constraints. This propagation was introduced in commit `aef6987d8954`, which added a `min_slice` field to `struct sched_entity` and used the RB-tree augmented callback mechanism (`min_vruntime_cb`) to maintain `min_slice` as the minimum of all `slice` values in each subtree. The `cfs_rq_min_slice()` function reads the root of the RB-tree's `min_slice` (combined with the current entity's slice) to determine the effective minimum slice for the entire `cfs_rq`, which is then assigned to the parent group entity's `se->slice`.

The bug occurs when a task is enqueued into (or dequeued from) a cgroup whose parent group sched_entity is already on the runqueue. In `enqueue_task_fair()` and `dequeue_entities()`, there are two `for_each_sched_entity()` loops. The first loop walks up the hierarchy and either enqueues entities that are not yet on the runqueue or breaks when it encounters an entity that is already on the runqueue. The second loop continues from that break point upward, updating load averages and propagating the `slice` value. However, in the second loop, the code assigned `se->slice = slice` without calling `min_vruntime_cb_propagate()` to update the augmented RB-tree data for that node. As a result, while the entity's `slice` field was updated to the new value, the RB-tree's `min_slice` subtree annotations were not recalculated, leaving parent and ancestor entities with stale `min_slice` information.

This means that when tasks with different slice values are enqueued or dequeued within a cgroup hierarchy, the root-level `cfs_rq`'s view of `min_slice` can be incorrect — it may reflect an outdated minimum slice rather than the true current minimum. This leads to parent group entities receiving slice values that do not match the actual requirements of their child entities.

The issue was identified by Tianchen Ding from Alibaba, who traced the problem to the missing propagation call in the second `for_each_sched_entity()` loop of both `enqueue_task_fair()` and `dequeue_entities()`. The fix was reviewed and tested by K Prateek Nayak from AMD.

## Root Cause

The root cause lies in how the `min_slice` augmented data is maintained in the RB-tree during enqueue and dequeue operations involving cgroup hierarchies.

When a task is enqueued via `enqueue_task_fair()`, the code walks up the sched_entity hierarchy. The first `for_each_sched_entity()` loop handles entities that need to be inserted into their parent `cfs_rq`:

```c
for_each_sched_entity(se) {
    if (se->on_rq) {
        if (se->sched_delayed)
            requeue_delayed_entity(se);
        break;  // <-- breaks when parent is already on_rq
    }
    cfs_rq = cfs_rq_of(se);
    ...
    enqueue_entity(cfs_rq, se, flags);       // inserts into RB-tree, propagates min_slice
    slice = cfs_rq_min_slice(cfs_rq);        // reads updated min_slice
    ...
}
```

When `enqueue_entity()` inserts an entity into the RB-tree via `__enqueue_entity()`, it calls `rb_add_augmented_cached()` which invokes `min_vruntime_cb` to properly propagate `min_slice` up the tree. So for entities processed in the first loop, `min_slice` is correctly propagated.

The second loop handles entities that are already on the runqueue (the hierarchy above the `break` point):

```c
for_each_sched_entity(se) {
    cfs_rq = cfs_rq_of(se);
    update_load_avg(cfs_rq, se, UPDATE_TG);
    se_update_runnable(se);
    update_cfs_group(se);

    se->slice = slice;                       // <-- updates slice value
    // MISSING: min_vruntime_cb_propagate()   // <-- does NOT propagate to RB-tree
    slice = cfs_rq_min_slice(cfs_rq);        // <-- reads stale min_slice
    ...
}
```

The assignment `se->slice = slice` updates the entity's own slice field, but this entity is already in the RB-tree. The augmented data (`se->min_slice`) in the RB-tree node is computed as `min(se->slice, left->min_slice, right->min_slice)` by `min_vruntime_update()`. Since `se->slice` changed but `min_vruntime_update()` was not called to recompute `se->min_slice`, the node's `min_slice` and all ancestor nodes' `min_slice` values in the RB-tree remain stale.

The function `min_vruntime_cb_propagate(&se->run_node, NULL)` is the RB-tree augmented callback that walks up from the given node to the root, calling `min_vruntime_update()` at each level to recompute the augmented `min_slice` (and `min_vruntime`). Without this call, the change to `se->slice` is invisible to `cfs_rq_min_slice()`, which reads `root->min_slice` from the RB-tree root node.

The same problem exists in `dequeue_entities()`. When a task is dequeued and its parent cgroup entity still has other runnable children (i.e., `cfs_rq->load.weight` is non-zero), the parent entity is not dequeued. The code breaks out of the first loop and enters the second loop, where again `se->slice = slice` is set without calling `min_vruntime_cb_propagate()`.

The condition `if (se != cfs_rq->curr)` in the fix is necessary because the currently running entity (`cfs_rq->curr`) is not in the RB-tree — it was dequeued from the tree when it was picked to run. Calling `min_vruntime_cb_propagate()` on a node that is not in the tree would be incorrect. For `curr`, the slice update is still written to `se->slice`, and `cfs_rq_min_slice()` already accounts for `curr->slice` separately via `if (curr && curr->on_rq) min_slice = curr->slice`.

## Consequence

The primary consequence is **incorrect scheduling time-slice assignment** for group sched_entities in a cgroup hierarchy. When `min_slice` is not properly propagated, parent group entities may be assigned a slice value that is too large, causing them to receive less frequent scheduling opportunities than their child entities require.

Specifically, consider a cgroup hierarchy where a task with a small custom slice (e.g., 1ms) is enqueued into a child cgroup. The `min_slice` of the child `cfs_rq` should propagate up so that the parent group entity gets a slice of 1ms, ensuring it is scheduled frequently enough to service the latency-sensitive child task. Without the propagation fix, the parent group entity's slice may remain at a default or previously computed value (e.g., 3ms or larger), meaning the parent gets scheduled less frequently. This introduces additional scheduling latency for the child task — it cannot run until its parent group entity is picked, and the parent's larger slice means it is picked less often by the EEVDF algorithm.

In the dequeue direction, when a task with the smallest slice in a cgroup is dequeued, the parent's `min_slice` should increase to reflect the next-smallest remaining task. Without propagation, the parent retains the old (too-small) minimum, potentially causing unnecessary preemptions and context switches, wasting CPU cycles.

While this bug does not cause a crash, kernel panic, or data corruption, it can lead to **measurable latency regressions** and **scheduling fairness violations** in workloads that rely on mixed slice configurations within cgroup hierarchies. This is particularly relevant for container environments (e.g., Kubernetes) where different workloads with different latency requirements share cgroup hierarchies. A related but more severe bug in the same `min_slice` propagation mechanism — where `se->slice` could be set to `U64_MAX` — was fixed separately in commit `bbce3de72be5` and could cause outright crashes.

## Fix Summary

The fix adds a call to `min_vruntime_cb_propagate(&se->run_node, NULL)` in the second `for_each_sched_entity()` loop of both `enqueue_task_fair()` and `dequeue_entities()`, immediately after the `se->slice = slice` assignment. This call triggers the augmented RB-tree propagation mechanism, which walks from the modified node up to the root of the RB-tree, recomputing `min_slice` (and `min_vruntime`) at each level. This ensures that when `cfs_rq_min_slice(cfs_rq)` is called on the next line, it reads a correct and up-to-date `min_slice` from the RB-tree root.

The propagation is guarded by `if (se != cfs_rq->curr)` because the currently running entity is not in the RB-tree. When an entity is picked to run, it is removed from the RB-tree (`__dequeue_entity()`), so calling `min_vruntime_cb_propagate()` on its `run_node` would operate on a node that is not linked into the tree structure, which is undefined behavior. For `cfs_rq->curr`, the `cfs_rq_min_slice()` function already handles it separately: `if (curr && curr->on_rq) min_slice = curr->slice`. So the slice update to `se->slice` is still effective for `curr` — it just does not need RB-tree propagation since `curr` is not in the tree.

The fix is minimal (4 lines of code: 2 lines added in each of the two locations) and precisely targets the missed propagation. It is correct because `min_vruntime_cb_propagate()` is exactly the function designed for this purpose — it is the RB-tree augmented callback propagation function generated by `RB_DECLARE_CALLBACKS()`, and it ensures that augmented data is consistent after a node's base value changes.

## Triggering Conditions

The bug requires:

- **CONFIG_FAIR_GROUP_SCHED enabled** — the bug only manifests with cgroup-based group scheduling, since the second `for_each_sched_entity()` loop only executes when there is a cgroup hierarchy (i.e., there are parent group entities above the task-level entity).
- **At least 2 levels of cgroup hierarchy** — a task must be in a child cgroup such that its parent cgroup's sched_entity is already on the runqueue when the task is enqueued. This means the parent cgroup must have at least one other runnable entity (either another task or another child group entity) so that the parent is not being freshly enqueued/dequeued.
- **Tasks with different slice values** — the bug is observable only when the `min_slice` value should change as a result of the enqueue or dequeue. If all tasks have the same default slice, the propagation miss is invisible. The bug becomes apparent when:
  - A task with a smaller-than-default slice is enqueued into a cgroup that already has tasks with larger slices running (enqueue path).
  - A task with the smallest slice in a cgroup is dequeued, and the remaining tasks have larger slices (dequeue path).
- **The parent group entity must already be on_rq** — if the parent entity is not on the runqueue (i.e., the child being enqueued is the first entity in this cgroup), the first loop handles the enqueue via `enqueue_entity()`, which properly propagates via `rb_add_augmented_cached()`. The bug only occurs in the second loop, which runs for entities that are already `on_rq`.
- **The parent group entity must not be cfs_rq->curr** — if the parent entity is the currently running entity, the fix's guard condition `se != cfs_rq->curr` skips propagation anyway (and it's correct to skip since curr is not in the tree).

The bug is deterministic — it occurs every time the above conditions are met. There is no race condition or timing dependency. The probability of triggering depends on workload characteristics: any workload with mixed slice values in a cgroup hierarchy will consistently trigger it.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP using the following approach. The key insight is to create a cgroup hierarchy where tasks with different slice values are enqueued, and then verify that the parent group entity's slice is correctly updated.

### Step 1: Setup

Configure QEMU with at least 2 CPUs. Pin all test tasks to CPU 1 (CPU 0 is reserved for the driver).

### Step 2: Create Cgroup Hierarchy

Use `kstep_cgroup_create("test_group")` to create a child cgroup. This provides a 2-level hierarchy: root cgroup → test_group cgroup.

### Step 3: Create Tasks with Different Slices

Create two CFS tasks:
1. **Task A** with a large (default) slice — this will be enqueued first to establish the parent group entity on the runqueue.
2. **Task B** with a small custom slice — this will be enqueued second, triggering the second `for_each_sched_entity()` loop where the bug manifests.

To set a custom slice on a task, use `kstep_sysctl_write()` to write to `/proc/sys/kernel/sched_min_granularity_ns` before the task is created, or use `KSYM_IMPORT` to access internal scheduler state and directly modify `se->slice` after creation but before wakeup. Alternatively, create tasks and use the `base_slice_ns` sysctl to control the default slice size, then directly write the task's `se->slice` field through internal access.

A more practical approach: use `kstep_sysctl_write("kernel/sched_base_slice_ns", "%llu", large_slice)` to set a large base slice, create and wake Task A in the cgroup, then use `kstep_sysctl_write("kernel/sched_base_slice_ns", "%llu", small_slice)` to set a small base slice, create and wake Task B in the cgroup. When Task B is enqueued, its `se->slice` will be the small value, and this should propagate up to the parent group entity.

### Step 4: Observe the Bug

After both tasks are enqueued:
1. Use `KSYM_IMPORT` and internal scheduler access (`cpu_rq(1)`, etc.) to read the parent group entity's `se->slice` and `se->min_slice` fields.
2. Read the root cfs_rq's `min_slice` via `cfs_rq_min_slice()` or by reading `__pick_root_entity(cfs_rq)->min_slice`.
3. On the **buggy kernel**: the parent group entity's `slice` will be set to the value propagated from the child `cfs_rq`, but the RB-tree's `min_slice` at the root of the parent's `cfs_rq` will NOT reflect this updated value. Specifically, the root cfs_rq (where the group entity sits) will have a stale `min_slice` that does not account for Task B's small slice.
4. On the **fixed kernel**: after Task B's enqueue, `min_vruntime_cb_propagate()` is called, and the root cfs_rq's RB-tree `min_slice` will correctly reflect the small slice.

### Step 5: Detection Logic

```
// After both tasks are enqueued in the child cgroup on CPU 1:
struct rq *rq = cpu_rq(1);
struct cfs_rq *root_cfs_rq = &rq->cfs;  // root cfs_rq
struct sched_entity *group_se = ...; // the test_group's se on CPU 1
struct sched_entity *root_entity = __pick_root_entity(root_cfs_rq);

// The group entity's slice should be the min_slice of its owned cfs_rq
// (which is the small slice from Task B)
u64 group_slice = group_se->slice;

// The root RB-tree's min_slice should reflect this
u64 tree_min_slice = root_entity ? root_entity->min_slice : ~0ULL;

// On buggy kernel: tree_min_slice may be larger than group_slice
//   because the propagation was missed
// On fixed kernel: tree_min_slice <= group_slice
```

Use `kstep_pass()` if `tree_min_slice` correctly reflects the small slice, `kstep_fail()` if it is stale.

### Step 6: Enqueue/Dequeue Sequence

1. `kstep_task_create()` → Task A, `kstep_task_pin(A, 1, 2)`, `kstep_cgroup_add_task("test_group", A->pid)`
2. Write large base_slice sysctl, `kstep_task_wakeup(A)`, `kstep_tick()` to let it get scheduled.
3. `kstep_task_create()` → Task B, `kstep_task_pin(B, 1, 2)`, `kstep_cgroup_add_task("test_group", B->pid)`
4. Write small base_slice sysctl (or directly modify `B->se.slice` via KSYM_IMPORT).
5. `kstep_task_wakeup(B)` — this triggers `enqueue_task_fair()` for Task B. Since Task A is already running in the cgroup, the parent group entity is on_rq. The second `for_each_sched_entity()` loop executes, assigning `se->slice = slice` to the parent group entity without propagation (on buggy kernel).
6. Immediately after wakeup, inspect the RB-tree `min_slice` of the root cfs_rq to verify correct propagation.

### Step 7: Also Test Dequeue Path

7. Block Task B (`kstep_task_block(B)`). On the buggy kernel, the parent group entity's `min_slice` in the RB-tree will remain at the small value instead of being updated to Task A's larger value.
8. Inspect the root cfs_rq's `min_slice` again — on the fixed kernel it should reflect only Task A's (larger) slice; on the buggy kernel it may still show the small slice.

### kSTEP Modifications Needed

Access to the group entity's internal fields (`se->slice`, `se->min_slice`) and the RB-tree root entity requires `KSYM_IMPORT` for internal symbols and direct access to `struct cfs_rq`, `struct sched_entity` through `kmod/internal.h`. The `on_sched_group_alloc` callback can be used to capture the task group's per-cpu sched_entity pointer when the cgroup is created. No fundamental changes to kSTEP are required — only internal symbol access which is already supported.

To set different slice values for tasks, one approach is to modify `se->slice` directly using internal access before waking the task. Alternatively, changing `sysctl_sched_base_slice` between task creations works since new tasks inherit the current base slice. The `kstep_sysctl_write` API already supports this.
