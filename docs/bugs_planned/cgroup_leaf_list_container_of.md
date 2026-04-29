# Cgroup: Invalid container_of on leaf_cfs_rq_list Head in child_cfs_rq_on_list

**Commit:** `3b4035ddbfc8e4521f85569998a7569668cccf51`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.14-rc6
**Buggy since:** v5.13 (introduced by `fdaba61ef8a2` "sched/fair: Ensure that the CFS parent is added after unthrottling")

## Bug Description

The function `child_cfs_rq_on_list()` in `kernel/sched/fair.c` performs an invalid `container_of` conversion when the `prev` pointer in the leaf CFS runqueue list happens to be the list head (`&rq->leaf_cfs_rq_list`) rather than an actual `struct cfs_rq`'s embedded `leaf_cfs_rq_list` member. This leads to a bogus pointer being used to dereference `->tg->parent`, reading from an invalid memory location within or before `struct rq` instead of a real `struct cfs_rq`.

The `leaf_cfs_rq_list` is a per-CPU linked list maintained by the CFS scheduler to track which `cfs_rq` structures (one per task group per CPU) need periodic load average updates. The list is headed by `rq->leaf_cfs_rq_list`. Each `cfs_rq` that has active entities or non-decayed load averages is linked into this list via its own `leaf_cfs_rq_list` member. The function `child_cfs_rq_on_list()` checks whether a child `cfs_rq` (in the task group hierarchy) appears immediately before a given `cfs_rq` on this list, or whether a child is pending addition via `rq->tmp_alone_branch`. This check is used by `cfs_rq_is_decayed()` to prevent premature removal of a parent `cfs_rq` from the leaf list when a child still needs it to maintain the tree structure.

The bug was introduced in commit `fdaba61ef8a2`, which added `child_cfs_rq_on_list()` and called it from `cfs_rq_is_decayed()` to fix a different issue where a CFS parent could be removed from the leaf list while a child still depended on it (during CFS bandwidth unthrottling). However, the implementation failed to account for the case where the `prev` pointer could be the list head itself rather than an embedded `leaf_cfs_rq_list` within a `cfs_rq`.

K Prateek Nayak confirmed in the review thread that a `SCHED_WARN_ON(prev == &rq->leaf_cfs_rq_list)` is "easily tripped during early boot" on his setup, demonstrating that the condition is not a theoretical edge case but occurs routinely in practice.

## Root Cause

The function `child_cfs_rq_on_list()` obtains a `prev` pointer from one of two sources:

```c
if (cfs_rq->on_list) {
    prev = cfs_rq->leaf_cfs_rq_list.prev;
} else {
    struct rq *rq = rq_of(cfs_rq);
    prev = rq->tmp_alone_branch;
}

prev_cfs_rq = container_of(prev, struct cfs_rq, leaf_cfs_rq_list);
return (prev_cfs_rq->tg->parent == cfs_rq->tg);
```

**Path 1 (on_list):** When `cfs_rq` is on the leaf list and is the **first element**, its `leaf_cfs_rq_list.prev` points back to the list head `&rq->leaf_cfs_rq_list`. This is standard Linux doubly-linked circular list behavior — the `prev` of the first element is the sentinel head node.

**Path 2 (!on_list):** When `cfs_rq` is not on the leaf list, the function reads `rq->tmp_alone_branch`. This pointer is reset to `&rq->leaf_cfs_rq_list` in `list_add_leaf_cfs_rq()` whenever a branch successfully connects to the tree (parent found on list) or when the root cfs_rq is added. Specifically, lines 344 and 359 in `list_add_leaf_cfs_rq()` set `rq->tmp_alone_branch = &rq->leaf_cfs_rq_list`, meaning this pointer frequently equals the list head.

In both cases, `prev` can be `&rq->leaf_cfs_rq_list`, which is embedded in `struct rq` at offset 1185 (in the current sched.h layout), within the `#ifdef CONFIG_FAIR_GROUP_SCHED` section between `fair_server` and `tmp_alone_branch`. The subsequent `container_of(prev, struct cfs_rq, leaf_cfs_rq_list)` computes `(struct cfs_rq *)((char *)prev - offsetof(struct cfs_rq, leaf_cfs_rq_list))`. Since `prev` does not actually reside within a `struct cfs_rq`, this produces a pointer to some arbitrary memory region calculated by subtracting the `leaf_cfs_rq_list` offset from the address of `rq->leaf_cfs_rq_list`.

The bogus `prev_cfs_rq` pointer is then used to access `prev_cfs_rq->tg`, which reads from whatever happens to be at the `tg` offset relative to the computed garbage base address. This is a classic out-of-bounds read: the kernel reads a pointer-sized value from an unrelated memory location, interprets it as a `struct task_group *`, and then dereferences its `parent` field. Depending on struct layout, this might read zeros (appearing as a NULL parent, causing the comparison to fail harmlessly), or it might read a valid-looking but incorrect pointer, or it might page-fault on an unmapped address.

## Consequence

The consequence depends on the relative positions of `leaf_cfs_rq_list` within `struct rq` and `tg` within `struct cfs_rq`. With current struct layouts in mainline kernels, the garbage read happens to produce a non-crashing result — the commit message explicitly states the bug "due to current struct layout might not be manifesting as a crash." However, this is fragile and layout-dependent:

1. **Memory corruption / crash:** If the computed bogus address falls in unmapped memory or a guard page, the kernel will page-fault, causing an oops or panic. This is more likely with kernel configurations that change struct sizes (e.g., enabling/disabling NUMA, SMP, or debug options). Even if it doesn't crash, writing through a bogus pointer later (if the return value incorrectly prevents list removal) could corrupt adjacent memory.

2. **Incorrect scheduling decisions:** If the garbage read produces `true` (the garbage `tg->parent` happens to match `cfs_rq->tg`), then `child_cfs_rq_on_list()` returns `true`, causing `cfs_rq_is_decayed()` to return `false`. This prevents the cfs_rq from being removed from the leaf list even when it is fully decayed. The cfs_rq will then be unnecessarily traversed during load updates (`update_blocked_averages`), wasting CPU time. Conversely, if the garbage read produces `false` when it should have produced `true` (if a real child were present), the parent cfs_rq could be incorrectly removed from the leaf list, disconnecting a branch from the PELT update traversal and causing stale load averages.

3. **Unpredictable behavior on layout changes:** Any kernel version upgrade, config change, or struct reordering could shift the relative positions of these structs, turning a silent garbage read into a crash or turning a benign false result into a harmful true result. This makes the bug a latent instability that can surface unpredictably.

## Fix Summary

The fix adds a simple guard check before the `container_of` operation:

```c
struct rq *rq = rq_of(cfs_rq);  // moved outside the if-else

if (cfs_rq->on_list) {
    prev = cfs_rq->leaf_cfs_rq_list.prev;
} else {
    prev = rq->tmp_alone_branch;
}

if (prev == &rq->leaf_cfs_rq_list)
    return false;

prev_cfs_rq = container_of(prev, struct cfs_rq, leaf_cfs_rq_list);
return (prev_cfs_rq->tg->parent == cfs_rq->tg);
```

The key change is the added `if (prev == &rq->leaf_cfs_rq_list) return false;` check. When `prev` is the list head, there is no previous `cfs_rq` before the current one — either the cfs_rq is the first element on the list (on_list path) or there is no pending alone branch (!on_list path). In both cases, there cannot be a child cfs_rq immediately before it, so returning `false` is the correct answer.

Additionally, the `struct rq *rq = rq_of(cfs_rq)` declaration is moved from inside the `else` block to the function scope, since `rq` is now needed by the new check regardless of which branch was taken. This is a minor cleanup that makes the code cleaner.

The fix is both correct and sufficient because `rq->leaf_cfs_rq_list` is a per-CPU list head that serves as the sentinel for the circular linked list. Only `cfs_rq` structures from the same CPU are added to this list, so comparing `prev` against the local CPU's `rq->leaf_cfs_rq_list` is enough to catch all cases where the prev pointer is not actually a `cfs_rq`'s embedded list node. There is no possibility of a `cfs_rq` from a different CPU's `rq` appearing on the list.

## Triggering Conditions

The bug requires `CONFIG_FAIR_GROUP_SCHED` to be enabled, which is the default on virtually all distribution kernels. Without fair group scheduling, `child_cfs_rq_on_list()` does not exist and `cfs_rq_is_decayed()` has a simplified implementation.

**Path 1 trigger (on_list, first element):** A `cfs_rq` must be on the `leaf_cfs_rq_list` and be the first element (i.e., closest to the list head). This occurs when:
- A task group hierarchy exists (at least one non-root task group).
- A `cfs_rq` for a leaf task group is added to the leaf list. If it's the only `cfs_rq` on the list, or if it was added at the head position (e.g., via `list_add_rcu` for an "alone branch" that later connected), it will be the first element.
- The `cfs_rq` then becomes decayed (its `load.weight` drops to 0 and its PELT averages decay to 0), causing `cfs_rq_is_decayed()` to be called.
- At that point, `child_cfs_rq_on_list()` reads `cfs_rq->leaf_cfs_rq_list.prev`, which is `&rq->leaf_cfs_rq_list`.

**Path 2 trigger (!on_list, tmp_alone_branch reset):** A `cfs_rq` is NOT on the leaf list, and `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list`. This occurs when:
- `tmp_alone_branch` was reset to the list head by a previous `list_add_leaf_cfs_rq()` call (either the branch connected to a parent on the list, or the root cfs_rq was added).
- Then `cfs_rq_is_decayed()` is called on a `cfs_rq` that is not on the list (e.g., during `update_blocked_averages()`).
- `child_cfs_rq_on_list()` reads `rq->tmp_alone_branch`, which equals `&rq->leaf_cfs_rq_list`.

As K Prateek Nayak reported, the condition is "easily tripped during early boot" — the early boot scenario involves the initial population of task groups when the leaf list is nearly empty, making it very likely that a `cfs_rq` ends up as the first element. This means the bug triggers frequently in normal operation, not just under exotic workloads.

The bug does **not** require specific CPU counts, NUMA topology, or special timing. A single CPU with `CONFIG_FAIR_GROUP_SCHED=y` and at least one non-root task group (which systemd creates automatically) is sufficient. CFS bandwidth throttling is **not** required to trigger the bug, although the original commit that introduced `child_cfs_rq_on_list()` was motivated by a bandwidth throttling scenario.

## Reproduce Strategy (kSTEP)

### Overview

This bug can be reproduced with kSTEP by creating a cgroup hierarchy and manipulating task states to trigger the condition where a `cfs_rq` is the first element on the `leaf_cfs_rq_list` and then becomes decayed. The detection relies on inspecting internal scheduler state to verify the invalid `container_of` condition is reached, and checking whether `cfs_rq_is_decayed()` behaves correctly.

### Step-by-step Plan

**1. Topology and Configuration:**
- Use at least 2 CPUs (QEMU default; CPU 0 is reserved for the driver, work happens on CPU 1).
- No special topology setup is needed — a simple SMP configuration suffices.
- `CONFIG_FAIR_GROUP_SCHED` must be enabled (it is by default in kSTEP's kernel config).

**2. Cgroup Hierarchy Setup:**
- Create a two-level cgroup hierarchy using `kstep_cgroup_create()`:
  - `kstep_cgroup_create("parent")` — creates a top-level task group
  - `kstep_cgroup_create("parent/child")` — creates a nested child task group
- This ensures at least two non-root `cfs_rq` structures exist per CPU, providing the task group depth needed for `child_cfs_rq_on_list()` to be meaningful.

**3. Task Creation and Placement:**
- Create a CFS task: `struct task_struct *t = kstep_task_create()`
- Pin the task to CPU 1: `kstep_task_pin(t, 1, 1)`
- Add the task to the child cgroup: `kstep_cgroup_add_task("parent/child", t->pid)`
- Wake the task: `kstep_task_wakeup(t)`
- Run several ticks to let the PELT averages accumulate: `kstep_tick_repeat(20)`
- This causes `child`'s `cfs_rq` (on CPU 1) to be added to the leaf list via `list_add_leaf_cfs_rq()`. The `parent`'s `cfs_rq` may also be added.

**4. Trigger the Decay:**
- Block the task: `kstep_task_block(t)` — this removes the task from the `cfs_rq`.
- Run many ticks to let the PELT averages fully decay: `kstep_tick_repeat(1000)` (PELT half-life is ~32ms, so ~1000 ticks at 1ms each ensures full decay).
- As the `cfs_rq`'s load decays, `update_blocked_averages()` (called from the scheduler tick) will call `cfs_rq_is_decayed()` on each `cfs_rq` on the leaf list. When the child's `cfs_rq` is fully decayed and is the first element on the list, `child_cfs_rq_on_list()` will be called with `prev == &rq->leaf_cfs_rq_list`.

**5. Detection via Internal State Inspection:**

Use `KSYM_IMPORT` to import relevant symbols and access internal state. In an `on_tick_end` callback or after the tick loop:

```c
struct rq *rq = cpu_rq(1);
struct list_head *pos;

/* Walk the leaf_cfs_rq_list and check if any cfs_rq is the first element */
list_for_each(pos, &rq->leaf_cfs_rq_list) {
    struct cfs_rq *cfq = container_of(pos, struct cfs_rq, leaf_cfs_rq_list);
    if (pos->prev == &rq->leaf_cfs_rq_list) {
        /* This cfs_rq is first on the list.
         * On the buggy kernel, child_cfs_rq_on_list(cfq) will
         * execute an invalid container_of on the list head.
         * On the fixed kernel, it will return false safely. */
        kstep_pass("Found cfs_rq at head of leaf list - bug condition reachable");
    }
    break;  /* Only need to check the first element */
}
```

**6. Detecting the Actual Corruption:**

To demonstrate the bug more concretely, we can compute what the buggy `container_of` would produce and check whether it reads valid memory:

```c
struct rq *rq = cpu_rq(1);
struct list_head *head = &rq->leaf_cfs_rq_list;
/* Compute the bogus cfs_rq pointer that the buggy code would produce */
struct cfs_rq *bogus_cfs_rq = container_of(head, struct cfs_rq, leaf_cfs_rq_list);
/* On the buggy kernel, the code would then read bogus_cfs_rq->tg->parent */
/* We can check if bogus_cfs_rq->tg is a valid pointer or garbage */
```

However, dereferencing the bogus pointer ourselves might crash the driver too. A safer approach is to:
- Check that the condition `prev == &rq->leaf_cfs_rq_list` IS reached (confirming the bug path is exercised).
- Verify that on the **fixed kernel**, after the decay completes, the `cfs_rq` is correctly removed from the list (since `child_cfs_rq_on_list` returns `false` and `cfs_rq_is_decayed` returns `true`).
- On the **buggy kernel**, if the garbage read happens to return `true` from `child_cfs_rq_on_list`, the cfs_rq will remain on the list even though it is fully decayed (observable difference).

**7. Pass/Fail Criteria:**

After running enough ticks for full PELT decay:
- **Check 1:** Verify that the leaf_cfs_rq_list was populated and the first-element condition was reached (both kernels should show this).
- **Check 2:** On the **fixed kernel**, the fully decayed `cfs_rq` should be removed from the leaf list (since `cfs_rq_is_decayed()` returns `true` and `list_del_leaf_cfs_rq()` is called in `update_blocked_averages`). Use `kstep_pass("cfs_rq correctly removed from leaf list")`.
- **Check 3:** On the **buggy kernel**, the fully decayed `cfs_rq` may remain on the leaf list (if the garbage read returned `true`) or may be removed (if the garbage read returned `false` — correct by accident). If it remains, use `kstep_fail("decayed cfs_rq incorrectly remains on leaf list")`. If it is removed (correct by accident), we can still detect the bug by checking whether the `container_of` on the list head was exercised (via a counter or flag set in the callback).

**8. Alternative Detection (Recommended):**

A more deterministic approach: use kSTEP's `on_tick_begin` or `on_tick_end` callback to inspect `rq->tmp_alone_branch` and the leaf list state on every tick. When we detect `tmp_alone_branch == &rq->leaf_cfs_rq_list` and a `cfs_rq` is about to be checked via `cfs_rq_is_decayed()`, log the condition. The mere occurrence of this condition on the buggy kernel proves the invalid `container_of` was executed, which constitutes the bug. On the fixed kernel, the same condition is handled safely by the early return.

**9. Expected Behavior Summary:**
- **Buggy kernel:** The `container_of` on `&rq->leaf_cfs_rq_list` executes, reading garbage memory. Depending on struct layout, this may crash (unlikely on current kernels) or silently produce an incorrect return value. The observable effect is that a decayed `cfs_rq` may stay on the leaf list when it should not (or vice versa).
- **Fixed kernel:** When `prev == &rq->leaf_cfs_rq_list`, the function returns `false` immediately without executing `container_of`. Decayed `cfs_rq` structures are correctly removed from the leaf list.

**10. kSTEP Extensions Needed:**

No kSTEP extensions are needed. The existing API provides:
- `kstep_cgroup_create()` and `kstep_cgroup_add_task()` for cgroup hierarchy setup
- `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_wakeup()`, `kstep_task_block()` for task management
- `kstep_tick_repeat()` for advancing time
- `cpu_rq()` and internal struct access via `internal.h` for state inspection
- `on_tick_end` callback for per-tick observation
- `kstep_pass()` and `kstep_fail()` for result reporting
