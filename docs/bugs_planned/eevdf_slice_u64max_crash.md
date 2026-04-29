# EEVDF: se->slice Set to U64_MAX Causing NULL Deref Crash

**Commit:** `bbce3de72be56e4b5f68924b7da9630cc89aa1a8`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.15-rc4
**Buggy since:** `aef6987d8954` ("sched/eevdf: Propagate min_slice up the cgroup hierarchy"), merged in v6.12-rc1

## Bug Description

A code path in `dequeue_entities()` can set the `slice` field of a `sched_entity` to `U64_MAX` (0xFFFFFFFFFFFFFFFF), which corrupts subsequent EEVDF scheduling calculations and can lead to a NULL pointer dereference crash in `pick_next_entity()`.

The bug is triggered when `dequeue_entities()` is called to dequeue a **delayed group scheduling entity** (a cgroup entity whose dequeue was previously delayed because it was not eligible), and the entity's **parent's dequeue also needs to be delayed**. In this scenario, the local variable `slice` gets initialized to `U64_MAX` from `cfs_rq_min_slice()` applied to an empty cfs_rq, and this value is never corrected before being propagated to the parent entity's `se->slice` field in the second `for_each_sched_entity()` loop.

The `cfs_rq_min_slice()` function returns `~0ULL` (U64_MAX) when a cfs_rq has no queued entities and no current entity on the runqueue, because the initial value `min_slice = ~0ULL` is never reduced. A delayed group entity has no queued tasks (they have already left), so its `group_cfs_rq()` is empty, making this the exact condition that produces U64_MAX.

The bug was introduced by commit `aef6987d8954` which added the min_slice propagation mechanism up the cgroup hierarchy. Before that commit, group entity slices were not propagated through `dequeue_entities()`, so the problematic code path did not exist.

## Root Cause

In the **buggy** version of `dequeue_entities()`, when the initial `se` is not a task (i.e., it is a group entity), the code early-initializes `slice` from the group's own cfs_rq:

```c
} else {
    cfs_rq = group_cfs_rq(se);
    slice = cfs_rq_min_slice(cfs_rq);  // BUG: returns U64_MAX if empty
}
```

The `cfs_rq_min_slice()` function (line 818) initializes `min_slice = ~0ULL`, then only reduces it if there is a current entity on the rq or a root entity in the RB tree. For an empty cfs_rq (no tasks, no current), it returns `~0ULL`.

The first `for_each_sched_entity()` loop (line 7091 in fixed code) iterates up the entity hierarchy dequeuing entities. There are three ways the loop can break:

1. **`dequeue_entity()` returns false** (dequeue is delayed): The loop breaks without updating `slice`. In the buggy code, `slice` retains whatever value was set before the loop.
2. **`cfs_rq->load.weight` is non-zero** (parent has other children): `slice = cfs_rq_min_slice(cfs_rq)` is set correctly from the non-empty parent cfs_rq.
3. **Loop exhausts all entities**: Only if the entity reaches the top of the hierarchy.

The critical bug scenario unfolds as follows:

- **Step 1**: `dequeue_entities()` is called on a delayed group entity `se_child`. Since `se_child` is not a task, `slice = cfs_rq_min_slice(group_cfs_rq(se_child))`. The entity is delayed, meaning its internal cfs_rq is empty → `slice = U64_MAX`.
- **Step 2**: The first loop iteration successfully dequeues `se_child` from its parent's cfs_rq (via `dequeue_entity()`, which returns true because the `DEQUEUE_DELAYED` flag forces the dequeue).
- **Step 3**: After dequeuing `se_child`, if `se_child` was its parent's **only child**, then `cfs_rq->load.weight == 0`, so the loop does NOT break at the `cfs_rq->load.weight` check. Instead, it continues to the next iteration to try dequeuing the parent entity `se_parent`.
- **Step 4**: When `dequeue_entity()` is called on `se_parent`, if `se_parent` is sleeping and not eligible, the `DELAY_DEQUEUE` feature causes `dequeue_entity()` to **return false** (delayed). The loop breaks immediately — **without updating `slice`**.
- **Step 5**: `slice` is still `U64_MAX` from step 1.
- **Step 6**: The second `for_each_sched_entity()` loop (line 7130) continues from `se_parent` upward. On the first iteration, it executes `se->slice = slice`, setting `se_parent->slice = U64_MAX`.

This corrupted slice value then cascades through EEVDF calculations:

- **`update_entity_lag()`** (line 767): Uses `cfs_rq_max_slice()` to compute `limit = calc_delta_fair(max_slice, se)`. When `se->slice` is U64_MAX, `cfs_rq_max_slice()` returns U64_MAX, making `max_slice + TICK_NSEC` overflow to a small number. Then `calc_delta_fair()` on this overflowed value produces a tiny or negative `limit`. The `clamp(vlag, -limit, limit)` with a negative `limit` means `vlag > limit` is always true, so `se->vlag` gets clamped to the huge negative `limit`.
- **`place_entity()`** (line 5164): The corrupted `se->vlag` (huge negative) is scaled by `(load + w_i) / load`, which can overflow. The result is subtracted from `vruntime` at line 5249: `se->vruntime = vruntime - lag`. This produces an `se->vruntime` that is astronomically far from the cfs_rq's `avg_vruntime`.
- **`vruntime_eligible()` / `entity_eligible()`** (line 797): The eligibility check computes `(vruntime - cfs_rq->zero_vruntime) * load`. With the corrupted vruntime being so far from the average, this multiplication overflows, producing a wrong sign, causing the entity to be incorrectly deemed ineligible.
- **`pick_eevdf()`** (line 1010): If no entity is found eligible (because vruntimes are corrupted), the function returns NULL.
- **`pick_next_entity()`** (line 5542): Dereferences the NULL return at `se->sched_delayed`, causing a kernel crash.

## Consequence

The primary consequence is a **NULL pointer dereference crash** in `pick_next_entity()` at `se->sched_delayed` (line 5547 of `fair.c`), which occurs when `pick_eevdf()` returns NULL because all entities on the runqueue appear ineligible due to corrupted vruntimes.

The crash manifests as a kernel oops/panic on the CPU that attempts to schedule a task from the affected cfs_rq. Since this occurs in the core scheduler pick path (`pick_task_fair()` → `pick_next_entity()` → `pick_eevdf()`), it happens during context switching when the scheduler is trying to select the next task to run. The CPU cannot recover from this — it cannot schedule any task and the system will either panic or become completely unresponsive on that CPU.

The author (Omar Sandoval at Meta) noted that the `se->slice = U64_MAX` condition was observed on live production systems and was "usually benign" because the subsequent crash required additional conditions: the corrupted entity must later be re-enqueued onto a cfs_rq in a particular state such that the vruntime corruption propagates to the point where no entity appears eligible. However, when the crash did occur, it was catastrophic — a production kernel panic. The bug was diagnosed using `drgn` to inspect core dumps, which showed tell-tale huge vruntime ranges and bogus vlag values.

## Fix Summary

The fix removes the early initialization of `slice` from `group_cfs_rq(se)` before the first loop and instead sets `slice` at the exact point where the first loop breaks due to a delayed dequeue. Specifically:

**Before (buggy):**
```c
} else {
    cfs_rq = group_cfs_rq(se);
    slice = cfs_rq_min_slice(cfs_rq);  // Can be U64_MAX if empty
}

for_each_sched_entity(se) {
    ...
    if (!dequeue_entity(cfs_rq, se, flags)) {
        if (p && &p->se == se)
            return -1;
        break;  // slice NOT updated here!
    }
    ...
}
```

**After (fixed):**
```c
}  // No else clause — slice stays 0

for_each_sched_entity(se) {
    ...
    if (!dequeue_entity(cfs_rq, se, flags)) {
        if (p && &p->se == se)
            return -1;

        slice = cfs_rq_min_slice(cfs_rq);  // Set from CURRENT cfs_rq
        break;
    }
    ...
}
```

The key insight is that when `dequeue_entity()` returns false (dequeue delayed), `cfs_rq` at that point refers to the cfs_rq of the entity whose dequeue was just delayed — which is a non-empty cfs_rq (it still has the delayed entity on it). Therefore `cfs_rq_min_slice(cfs_rq)` will return a valid slice value, not U64_MAX. This ensures that the second `for_each_sched_entity()` loop always receives a sensible `slice` value to propagate.

The fix is correct and complete because it covers all three break paths from the first loop: (1) delayed dequeue now sets `slice` from the current (non-empty) cfs_rq, (2) the `cfs_rq->load.weight` path already set `slice` correctly, and (3) when the loop runs to completion `slice` remains 0 which is harmless since there are no more parent entities to update.

## Triggering Conditions

The bug requires the following precise conditions:

1. **CONFIG_FAIR_GROUP_SCHED must be enabled** (cgroup support for CFS). This is required for group scheduling entities to exist. Without it, all entities are tasks and the `else` branch that initializes `slice` from `group_cfs_rq()` is never reached.

2. **CONFIG_SCHED_DEBUG with DELAY_DEQUEUE feature enabled** (default on modern kernels). The `DELAY_DEQUEUE` sched feature must be active for `dequeue_entity()` to return false (delay the dequeue). This feature is enabled by default in kernels v6.12+.

3. **A cgroup hierarchy at least 3 levels deep**: The bug requires at minimum a root cgroup → parent group → child group. The child group entity must be delayed, and after dequeuing it, the parent group entity's dequeue must also be delayed.

4. **The child group entity must be delayed with an empty internal cfs_rq**: This means the child group previously had tasks that were all dequeued, but the group entity itself was kept on the parent's cfs_rq in a delayed state (because it was not eligible at the time).

5. **The child group entity must be the only child of its parent group**: After dequeuing the child entity, the parent's cfs_rq must become empty (`cfs_rq->load.weight == 0`) so the first loop continues to try dequeuing the parent rather than breaking at the `cfs_rq->load.weight` check.

6. **The parent entity's dequeue must be delayed**: When `dequeue_entity()` is called on the parent, the parent must be sleeping and not eligible, causing `DELAY_DEQUEUE` to trigger and return false.

7. **The corrupted entity must later be re-enqueued**: The entity with `slice = U64_MAX` must be woken up and placed on a cfs_rq, triggering `place_entity()` which uses the bogus `vlag` computed from the corrupted slice.

8. **Multiple tasks on the affected cfs_rq**: For the crash to manifest, `cfs_rq->nr_queued` must be > 1 (so `pick_eevdf()` doesn't short-circuit at the single-entity optimization at line 1021) and the corrupted vruntime must cause all entities to appear ineligible.

The probability of hitting the full crash chain is relatively low in any single scheduling operation, but in production environments with many cgroups and frequent task creation/destruction, the `slice = U64_MAX` condition occurs regularly. The author confirmed tracing it on live systems. The full crash requires the additional cascading conditions (steps 7-8) to align, making it an intermittent production crash.

## Reproduce Strategy (kSTEP)

The reproduction strategy requires creating a 3-level cgroup hierarchy and carefully orchestrating task dequeue timing to trigger the specific code path where a delayed group entity with an empty cfs_rq is dequeued, followed by its parent's dequeue being delayed.

### Setup

1. **CPU configuration**: Use at least 2 CPUs. Pin all test tasks to CPU 1 (CPU 0 is reserved for the driver).

2. **Cgroup hierarchy**: Create a 3-level hierarchy:
   - `/test_parent` — the parent cgroup
   - `/test_parent/test_child` — the child cgroup
   Set both to use the same cpuset containing CPU 1.

3. **Tasks**:
   - Create **Task A**: a CFS task placed in `/test_parent/test_child`. This task will be used to populate and then empty the child cgroup's cfs_rq.
   - Create **Task B**: a CFS task placed in `/test_parent` (but NOT in `test_child`). This task is needed temporarily to keep the parent entity from being fully dequeued too early. It will be removed at the right moment.
   - Create **Task C**: a CFS task placed in `/test_parent/test_child` or `/test_parent`. This task will be used to observe the corrupted vruntime/slice after the bug triggers.

### Triggering Sequence

4. **Phase 1 — Populate hierarchy**: Wake up Task A and Task B on CPU 1 and let them run for several ticks so that their vruntimes advance and they establish normal scheduling state.

5. **Phase 2 — Empty the child cfs_rq**: Block Task A (via `kstep_task_block()`). This removes the only task from `test_child`'s cfs_rq. If `DELAY_DEQUEUE` is active and Task A is not eligible at the time of blocking, Task A's dequeue may itself be delayed. Either way, the child group's internal cfs_rq becomes empty of running tasks. If the child group entity's dequeue is delayed, `test_child`'s group `sched_entity` remains on the parent's cfs_rq in a delayed state — exactly the state we need.

6. **Phase 3 — Remove sibling from parent**: Block Task B. Now `test_child`'s group entity is the only entity on `test_parent`'s cfs_rq. This ensures that when the child group entity is eventually dequeued, the parent will have `cfs_rq->load.weight == 0`, forcing the loop to continue to the parent.

7. **Phase 4 — Trigger the dequeue cascade**: The key is to cause `dequeue_entities()` to be called on the delayed child group entity. This happens naturally when the scheduler picks the delayed entity via `pick_next_entity()` → `pick_eevdf()`. Since delayed entities are still on the rbtree, they can be picked, and `pick_next_entity()` will call `dequeue_entities()` with `DEQUEUE_SLEEP | DEQUEUE_DELAYED`. Advance ticks (`kstep_tick_repeat()`) to trigger scheduling decisions on CPU 1. The delayed child entity should be picked, causing the cascading dequeue.

8. **Phase 5 — Check for corruption**: Use `KSYM_IMPORT` to access the parent group entity's `sched_entity` structure. After the dequeue cascade, read `se->slice` from the parent group entity. On the buggy kernel, it should be `U64_MAX` (0xFFFFFFFFFFFFFFFF). On the fixed kernel, it should be a reasonable value.

### Detection via Callbacks

9. **Use `on_tick_begin` or `on_tick_end` callback**: In the callback, access the parent group's `sched_entity` and check its `slice` field. If `se->slice == ~0ULL`, report failure via `kstep_fail()`.

10. **Alternative detection — observe the crash**: On the buggy kernel, after the parent entity with corrupted slice is re-enqueued (e.g., by waking Task A or Task C into the hierarchy), subsequent scheduling may trigger the NULL deref in `pick_next_entity()`. The driver can catch this by checking if `pick_eevdf()` returns NULL, but since the crash is in kernel code, it may manifest as a kernel oops. The driver should use `kstep_pass()` if scheduling continues normally after the dequeue cascade (fixed kernel) and `kstep_fail()` if the slice corruption is detected or a crash occurs (buggy kernel).

### Detailed Implementation Steps

11. **Access internal structures**: Use `KSYM_IMPORT` and the `internal.h` header to access:
    - `cpu_rq(1)` to get the runqueue for CPU 1
    - `task_group` structures for the cgroups
    - `group_cfs_rq()` and `cfs_rq_of()` to navigate the entity hierarchy
    - `se->slice`, `se->vlag`, `se->vruntime` fields for monitoring

12. **Timing control**: Use `kstep_tick()` and `kstep_sleep()` to precisely control when scheduling decisions occur. The key window is between blocking Task A (emptying child cfs_rq), blocking Task B (making child the only entity in parent), and the scheduler picking the delayed child entity.

13. **Ensuring delay dequeue triggers**: The DELAY_DEQUEUE feature delays dequeue when an entity is sleeping and not eligible. To ensure the child group entity gets delayed rather than immediately dequeued, it should have consumed more than its fair share of CPU time (i.e., its vruntime should be ahead of avg_vruntime, making it ineligible). Running Task A for many ticks before blocking it should achieve this. If `kstep_eligible()` returns false for the entity, the delay will trigger.

14. **Expected results**:
    - **Buggy kernel**: The parent group entity's `se->slice` will be set to `U64_MAX` after the dequeue cascade. If the entity is subsequently re-enqueued, `update_entity_lag()` will compute a bogus limit, corrupting `se->vlag`, which will corrupt `se->vruntime` in `place_entity()`, potentially leading to `pick_eevdf()` returning NULL and crashing.
    - **Fixed kernel**: The parent group entity's `se->slice` will be set to a valid value from `cfs_rq_min_slice()` of the parent's cfs_rq (which still contains the delayed parent entity). All subsequent calculations will use correct values.

### kSTEP Compatibility Notes

15. This bug is fully reproducible with kSTEP. The required capabilities are:
    - Cgroup creation and task assignment: `kstep_cgroup_create()`, `kstep_cgroup_add_task()`
    - Task lifecycle: `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_block()`, `kstep_task_wakeup()`
    - Tick control: `kstep_tick()`, `kstep_tick_repeat()`
    - Internal state access: `KSYM_IMPORT`, `cpu_rq()`, `cfs_rq` internals via `internal.h`
    - Eligibility checking: `kstep_eligible()`
    All of these are available in the current kSTEP framework. No extensions are needed.
