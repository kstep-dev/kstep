# EEVDF: pick_eevdf() Misses Earliest Eligible Deadline in Left Subtree

**Commit:** `b01db23d5923a35023540edc4f0c5f019e11ac7d`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.6-rc6
**Buggy since:** `147f3efaa241` ("sched/fair: Implement an EEVDF-like scheduling policy"), merged in v6.6-rc1

## Bug Description

The `pick_eevdf()` function, which selects the next runnable CFS task using the Earliest Eligible Virtual Deadline First policy, contains a logic error in its augmented RB-tree search that causes it to miss eligible entities with the earliest deadline under certain tree configurations. Specifically, when the algorithm descends into the right subtree chasing a `min_deadline` value, it can discover that the entity holding that `min_deadline` is actually ineligible. At that point, the algorithm has already passed by left subtrees that contain eligible entities with better (earlier) deadlines than the current best candidate, and it has no mechanism to go back and search those skipped branches.

The EEVDF scheduler maintains an augmented RB-tree (`cfs_rq->tasks_timeline`) where entities are sorted by `vruntime` (service received) and each node is augmented with `min_deadline`, the minimum deadline among all entities in its subtree. This augmented structure allows an EDF-like heap search in O(log n) time. The key invariant exploited by the search is that if a node is eligible (meaning `avg_vruntime(cfs_rq) >= se->vruntime`), then every entity in its left subtree is also eligible, because left children have smaller vruntimes and are therefore even more "owed service."

The bug exists from the initial EEVDF implementation in v6.6-rc1 through v6.6-rc5. It affects every scheduling decision on every CFS runqueue, though in practice it only produces an incorrect pick when the specific tree configuration arises—which depends on the mix of task weights, vruntimes, and deadlines.

The author (Benjamin Segall) noted that the bug was discovered through careful analysis and verified using a userspace stress test built on `tools/lib/rbtree.c`. The fix rewrites the search as a two-phase algorithm that correctly handles the case where `min_deadline` descends into ineligible territory.

## Root Cause

The buggy `pick_eevdf()` performs a single-pass descent of the augmented RB-tree with the following logic:

```c
while (node) {
    struct sched_entity *se = __node_2_se(node);

    if (!entity_eligible(cfs_rq, se)) {
        node = node->rb_left;
        continue;
    }

    if (!best || deadline_gt(deadline, best, se)) {
        best = se;
        if (best->deadline == best->min_deadline)
            break;
    }

    if (node->rb_left &&
        __node_2_se(node->rb_left)->min_deadline == se->min_deadline) {
        node = node->rb_left;
        continue;
    }

    node = node->rb_right;
}
```

The algorithm tries to find the eligible entity with the earliest deadline by combining an eligibility-filtered walk with a heap-based `min_deadline` search. At each eligible node:

1. It updates `best` if the current node has an earlier deadline.
2. If `best->deadline == best->min_deadline`, the current best IS the minimum-deadline entity in its subtree, so the search terminates.
3. If the left child's `min_deadline` equals the current node's `min_deadline`, the overall minimum is in the (fully eligible) left subtree, so it goes left.
4. Otherwise, it goes right, following the `min_deadline` deeper into the right subtree.

The critical flaw is in step 4. When the algorithm descends right, it's chasing a `min_deadline` value that originates from an entity deeper in the right subtree. But entities in the right subtree have higher vruntimes and may be **ineligible**. When the algorithm later encounters that ineligible entity, it goes left from it (`node = node->rb_left`), but this left turn does NOT lead back to the left subtrees that were skipped at previous eligible ancestors. The algorithm has effectively committed to the right branch and lost track of any potentially better eligible candidates in left branches encountered earlier.

Consider this concrete example. Let there be three entities A, B, C on the tree with `avg_vruntime ≈ 55`:

```
       B (vruntime=50, eligible, deadline=100, min_deadline=30)
      / \
     A   C (vruntime=80, ineligible, deadline=30, min_deadline=30)
  (vruntime=30,
   eligible,
   deadline=70,
   min_deadline=70)
```

The buggy algorithm proceeds:
1. Start at B. B is eligible. `best = B` (deadline=100). `B.deadline (100) != B.min_deadline (30)`, so don't break.
2. Check left: `A.min_deadline (70) != B.min_deadline (30)`. The overall `min_deadline` is NOT in the left subtree, so don't go left.
3. Go right to C.
4. C is ineligible. `node = C->rb_left = NULL`. Loop ends.
5. Return `best = B` with deadline=100.

**Correct answer:** A (eligible, deadline=70). The algorithm missed A because it went right chasing `min_deadline=30` (which belongs to ineligible entity C), and when C turned out to be ineligible, there was no path back to A.

The root cause is that the single-loop approach conflates two different search goals: (a) finding the best eligible entity, and (b) following `min_deadline` pointers in the heap. When the `min_deadline` leads to an ineligible entity, goal (b) fails, but the algorithm has already abandoned left subtrees that could satisfy goal (a).

## Consequence

The primary consequence is a **violation of the EEVDF scheduling policy**: the scheduler picks an eligible entity that does NOT have the earliest deadline, when a better candidate exists on the runqueue. This means tasks with tighter deadlines are denied their rightful scheduling priority.

The observable impacts include:

- **Increased tail latency:** Tasks that should run imminently (earliest deadline) are delayed in favor of tasks with later deadlines. For interactive or latency-sensitive workloads, this manifests as occasional unexpected scheduling delays—the scheduler "forgets" about a ready task with an urgent deadline and picks a less urgent one instead.
- **EEVDF fairness violation:** The entire purpose of EEVDF is to provide latency guarantees proportional to each task's weight (request size). When the wrong entity is picked, tasks with higher weight (which should get shorter latencies due to shorter slices and thus earlier deadlines) may be starved in favor of lower-weight tasks.
- **Potential cascading effects:** Incorrect picks shift the vruntime distribution, potentially causing further suboptimal picks in subsequent scheduling decisions.

In the worst case, if the tree configuration persistently triggers this bug (e.g., a stable set of tasks with specific weight ratios), a high-priority CFS task could repeatedly fail to be selected despite having the earliest eligible deadline, causing it to experience latencies far beyond its expected slice. The scheduler's built-in error detection (`pr_err("EEVDF scheduling fail, picking leftmost")`) can also trigger if `pick_eevdf` returns NULL, though this is an extreme case—the bug more commonly results in a suboptimal (non-NULL) pick.

## Fix Summary

The fix, authored by Benjamin Segall, completely rewrites the `pick_eevdf()` search as a two-phase algorithm in a new function `__pick_eevdf()`, which is then called by the wrapper `pick_eevdf()` that handles the error/fallback case.

**Phase 1 (first while loop):** Descend the tree following `min_deadline`, tracking two things separately:
- `best`: the best eligible entity seen so far (by deadline).
- `best_left`: the left-branch with the best `min_deadline` seen so far. Every entity in a left branch of an eligible ancestor is guaranteed to be eligible, so `best_left` represents a fully-eligible subtree containing a potentially better candidate.

The first loop descends right when `min_deadline` is in the right subtree, just as before. But crucially, at each eligible node, it also records whether the left child has a promising `min_deadline` (tracking it in `best_left`). The loop breaks when it determines the `min_deadline` is in the left branch (to switch to Phase 2) or when the current node IS the min_deadline node.

**Phase 2 (second while loop):** If `best_left` exists and `best_left->min_deadline < best->deadline`, then a better eligible candidate exists somewhere in `best_left`'s subtree. Since all entities in `best_left`'s subtree are eligible (they are descendants of a left child of an eligible node), the search simply needs to find the entity where `deadline == min_deadline`—a straightforward heap descent without any eligibility checks.

After Phase 2, the result is compared with `best` from Phase 1, and the overall winner is returned.

The fix is correct because it separates the two concerns that the buggy code conflated: (a) tracking eligible entities along the main descent path, and (b) knowing that left subtrees are fully eligible and can be searched more efficiently later. By deferring the search into left subtrees until after the main descent, the algorithm never loses track of potential candidates. The author verified correctness using a userspace stress test built on the kernel's own `tools/lib/rbtree.c`, exercising corner cases in the augmented tree search.

## Triggering Conditions

The bug is triggered when the following conditions are simultaneously met at a scheduling decision point:

1. **Multiple runnable CFS entities on the same runqueue:** At least 3 entities must be present on the CFS RB-tree. The tree needs enough structure to have both left and right subtrees with the specific properties described below.

2. **Mixed eligibility in the tree:** There must be at least one eligible entity and at least one ineligible entity. Eligibility is determined by `entity_eligible(cfs_rq, se)`, which checks whether `avg_vruntime(cfs_rq) >= se->vruntime` (adjusted by weight). This naturally occurs when tasks have different vruntimes due to different weights (nice values) or different execution histories.

3. **The overall `min_deadline` of an eligible ancestor must reside in its ineligible right subtree:** This means an ineligible entity (high vruntime) has a deadline lower than all eligible entities' deadlines. This occurs when a high-weight task (low nice value) has been running and accumulated high vruntime (becoming ineligible) while retaining a low deadline (because `deadline = vruntime_at_enqueue + short_slice`). High-weight tasks get short slices (`slice ∝ 1/weight`), so their deadlines are closer to their vruntimes.

4. **A better eligible entity exists in a left subtree that was skipped:** There must be an eligible entity in the left branch of some ancestor along the descent path, and this entity's deadline must be strictly earlier than the `best` entity found on the main descent path. This entity was skipped because the algorithm followed `min_deadline` to the right instead.

These conditions arise naturally in workloads with mixed nice values (and therefore mixed weights), because:
- High-weight tasks (low nice) get short slices and advance vruntime quickly per unit of real time, leading to high vruntimes and low deadlines.
- Low-weight tasks (high nice) get long slices, leading to high deadlines relative to their vruntimes.
- The combination creates exactly the tree topology where `min_deadline` points rightward to ineligible high-weight entities.

No special kernel configuration is needed beyond having EEVDF enabled (which is the default CFS scheduler from v6.6-rc1). The bug can trigger on any number of CPUs; it is per-runqueue. A single CPU with 3+ runnable CFS tasks is sufficient.

The probability of triggering depends on the workload mix. With 3 tasks of different nice values on the same CPU, the specific tree topology should appear after sufficient vruntime divergence (10-20 scheduling ticks). With more tasks and more weight diversity, the bug triggers more frequently. The bug is deterministic given a specific tree state—there is no race condition or timing dependency, only a tree-shape dependency.

## Reproduce Strategy (kSTEP)

The reproduction strategy creates a scenario where the EEVDF RB-tree develops the specific topology that triggers the incorrect pick, and then validates that the kernel's `pick_eevdf()` selects a suboptimal entity.

### Task Setup

Create 4-6 CFS tasks with varying nice values, all pinned to CPU 1 (avoiding CPU 0 which is reserved for the driver). Use a spread of nice values to create diverse weights and thus diverse slice lengths and vruntime advance rates:

- **Task A:** nice -10 (weight ~3121) — high weight, very short slice, vruntime advances slowly
- **Task B:** nice 0 (weight 1024) — normal weight and slice
- **Task C:** nice 5 (weight ~655) — lower weight, longer slice
- **Task D:** nice 10 (weight ~110) — very low weight, very long slice

All tasks should be created with `kstep_task_create()`, pinned to CPU 1 with `kstep_task_pin(task, 1, 1)`, and given appropriate nice values with `kstep_task_set_prio()`.

### Version Guard

The driver should be guarded with `#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 6, 0)` since both the buggy (v6.6-rc1 through rc5) and fixed (v6.6-rc6) kernels compile as version 6.6.0. The behavior difference between buggy and fixed kernels will be the test result.

### Execution Plan

1. **Wake all tasks** to enqueue them on CPU 1's CFS runqueue.
2. **Run for 15-25 ticks** (`kstep_tick_repeat(20)`) to let vruntimes diverge. High-weight tasks (Task A) will accumulate vruntime faster per tick but advance vruntime slower per unit of virtual time (due to weight scaling), while low-weight tasks (Task D) will advance vruntime faster per unit of execution.
3. **At a detection point**, examine the RB-tree state by walking `cpu_rq(1)->cfs.tasks_timeline` to find all entities and their eligibility/deadline status.

### Bug Detection: Brute-Force EED Verification

The core detection logic should implement a brute-force O(n) scan of the CFS RB-tree and compare against what the kernel actually picks. This is done in the `on_tick_end` callback (or at strategic points in `run()`):

1. **Walk the RB-tree:** Iterate through all nodes in `cfs_rq->tasks_timeline` using `rb_first()` / `rb_next()`.
2. **For each entity**, call `kstep_eligible(&se)` to check eligibility.
3. **Among eligible entities**, find the one with the earliest (minimum) `se->deadline` using signed comparison: `(s64)(se->deadline - best->deadline) < 0`.
4. **Also check `cfs_rq->curr`** if it is eligible (it's not in the tree but is a valid pick candidate).
5. **Compare** the brute-force result with `cfs_rq->curr` (the entity the kernel actually picked for execution).

If the brute-force minimum-deadline eligible entity differs from `cfs_rq->curr`, and `cfs_rq->curr` has a later deadline, the bug has been triggered.

### Callback Strategy

Use `on_tick_end` to perform the check after each tick's scheduling decision. The callback fires after the tick handler has completed, including any `schedule()` call triggered by `resched_curr()`. At this point, `cfs_rq->curr` reflects the kernel's actual pick.

```
on_tick_end:
  cfs_rq = &cpu_rq(1)->cfs
  correct_pick = brute_force_eed(cfs_rq)
  if correct_pick != NULL and cfs_rq->curr != NULL:
    if correct_pick != cfs_rq->curr:
      if (s64)(cfs_rq->curr->deadline - correct_pick->deadline) > 0:
        // Bug! Kernel picked entity with later deadline
        kstep_fail("pick_eevdf missed EED: picked deadline=%lld, optimal=%lld",
                    cfs_rq->curr->deadline, correct_pick->deadline)
```

### Increasing Trigger Probability

To maximize the chance of hitting the buggy tree topology:
- **Vary the number of running tasks over time:** Block and wake tasks at different intervals. This creates enqueue/dequeue events that reshape the tree.
- **Use `kstep_task_pause()` and `kstep_task_wakeup()`** to temporarily remove and re-add entities, creating churn in the vruntime distribution.
- **Adjust nice values dynamically** with `kstep_task_set_prio()` to change weights mid-run, which affects slice/deadline calculations at the next enqueue.

### Expected Results

- **Buggy kernel (v6.6-rc1 to v6.6-rc5):** The brute-force check should detect at least one instance where `cfs_rq->curr` does not have the earliest eligible deadline. `kstep_fail()` is called with details of the suboptimal pick.
- **Fixed kernel (v6.6-rc6+):** The two-phase `__pick_eevdf()` always finds the correct EED entity. The brute-force check should always agree with the kernel's pick. `kstep_pass()` is called after all ticks complete without discrepancy.

### Required Internal Access

The driver needs access to the following via `internal.h` (which includes `kernel/sched/sched.h`):
- `cpu_rq(cpu)` — to get the per-CPU runqueue
- `cfs_rq->tasks_timeline` — the augmented RB-tree
- `cfs_rq->curr` — the currently running entity
- `struct sched_entity` fields: `vruntime`, `deadline`, `min_deadline`, `on_rq`, `run_node`
- `kstep_eligible()` — to check entity eligibility (calls `entity_eligible` internally)
- `rb_first()`, `rb_next()`, `rb_entry()` — for tree iteration

No kSTEP framework extensions are needed. All required functionality is already available through `driver.h`, `internal.h`, and the existing `kstep_eligible()` API. The `KSYM_IMPORT` mechanism can be used to import `entity_eligible` directly if needed, though `kstep_eligible()` wraps it already.
