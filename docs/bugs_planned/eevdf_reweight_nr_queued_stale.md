# EEVDF: Stale nr_queued During reweight_entity Violates place_entity Constraints

**Commit:** `c70fc32f44431bb30f9025ce753ba8be25acbba3`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.16-rc1
**Buggy since:** v6.13 (introduced by commit `6d71a9c61604` "sched/fair: Fix EEVDF entity placement bug causing scheduling lag")

## Bug Description

When a scheduling entity is reweighted while on the runqueue (e.g., during a cgroup `cpu.weight` change), the `reweight_entity()` function in `kernel/sched/fair.c` calls `place_entity()` to re-place the entity with its new weight. However, `reweight_entity()` fails to decrement `cfs_rq->nr_queued` before calling `place_entity()`, leaving the counter stale. This causes `place_entity()` to see a `nr_queued` value that includes the entity being reweighted, even though it has been conceptually removed from the queue for the purposes of re-placement.

The bug was introduced by commit `6d71a9c61604` which rewrote `reweight_entity()` to use `place_entity()` for EEVDF placement instead of the old `reweight_eevdf()` function. The old code computed vruntime and deadline adjustments inline, while the new code properly delegates to `place_entity()` for consistency with other enqueue paths. However, the new code failed to maintain the `nr_queued` invariant that `place_entity()` relies on: when placing an entity, `nr_queued` should reflect the number of entities currently in the queue *excluding* the entity being placed.

The bug is masked in practice by a co-existing optimization from commit `4423af84b297` ("sched/fair: optimize the PLACE_LAG when se->vlag is zero"), which adds a `se->vlag` check to the PLACE_LAG condition in `place_entity()`. Since the last entity on a cfs_rq always has zero vlag (the average of a single element equals that element), this optimization causes `place_entity()` to skip the PLACE_LAG block, preventing the stale `nr_queued` from causing a `WARN_ON_ONCE`. However, this dependency on a separate optimization is fragile: if `4423af84b297` is ever reverted or not backported alongside `6d71a9c61604`, a kernel warning will fire.

## Root Cause

The root cause is that `reweight_entity()` does not properly bracket its call to `place_entity()` with `nr_queued` adjustments. In the buggy code path (commit `6d71a9c61604` through the parent of `c70fc32f`), `reweight_entity()` performs these steps when `se->on_rq` is true:

```c
if (se->on_rq) {
    update_curr(cfs_rq);
    update_entity_lag(cfs_rq, se);
    se->deadline -= se->vruntime;
    se->rel_deadline = 1;
    // BUG: nr_queued NOT decremented here
    if (!curr)
        __dequeue_entity(cfs_rq, se);
    update_load_sub(&cfs_rq->load, se->load.weight);
}
// ... scale vlag and deadline, set new weight ...
if (se->on_rq) {
    update_load_add(&cfs_rq->load, se->load.weight);
    place_entity(cfs_rq, se, 0);  // <-- called with stale nr_queued
    if (!curr)
        __enqueue_entity(cfs_rq, se);
    // BUG: nr_queued NOT incremented here
}
```

The `place_entity()` function uses `cfs_rq->nr_queued` in its PLACE_LAG condition:

```c
if (sched_feat(PLACE_LAG) && cfs_rq->nr_queued && se->vlag) {
    // Inflate lag to compensate for the effect of adding the entity
    // to the weighted average
    load = cfs_rq->avg_load;
    if (curr && curr->on_rq)
        load += scale_load_down(curr->load.weight);
    lag *= load + scale_load_down(se->load.weight);
    if (WARN_ON_ONCE(!load))
        load = 1;
    lag = div_s64(lag, load);
}
```

For the critical single-entity case: when the entity being reweighted is the only entity on the cfs_rq, `nr_queued` is 1 (not decremented). The entity's `vlag` is computed by `update_entity_lag()` as `avg_vruntime(cfs_rq) - se->vruntime`. Since the entity is the sole entity, `avg_vruntime` equals its own vruntime, so `vlag = 0`.

With the `se->vlag` optimization present (commit `4423af84b297`), the condition `cfs_rq->nr_queued && se->vlag` evaluates to `1 && 0 = false`, correctly skipping the PLACE_LAG block. Without that optimization, the condition would be just `cfs_rq->nr_queued`, which evaluates to `1 = true`. The code would then enter the PLACE_LAG block with `load = cfs_rq->avg_load = 0` (the tree is effectively empty after dequeue), triggering `WARN_ON_ONCE(!load)`.

Additionally, the buggy code calls `update_load_add()` *before* `place_entity()`, which is inconsistent with other `place_entity()` callers. While `place_entity()` does not use `cfs_rq->load` directly (it uses `cfs_rq->avg_load`), this ordering inconsistency is a code hygiene issue. The fix also corrects this ordering.

## Consequence

The primary consequence is a `WARN_ON_ONCE(!load)` kernel warning in `place_entity()` when the `se->vlag` optimization (commit `4423af84b297`) is absent. This can occur if:
1. A stable kernel backports commit `6d71a9c61604` without also backporting `4423af84b297`.
2. The `se->vlag` optimization is reverted for any reason.

When the WARN fires, the kernel prints a stack trace to the kernel log and sets `load = 1` as a fallback. This means the lag inflation calculation `lag = lag * (load + se_weight) / load` uses an incorrect denominator, potentially producing a wildly inflated lag value. This could cause the entity's `vruntime` to be placed far behind the queue's average, giving the reweighted entity an unfair scheduling advantage (it would appear to have run much less than it actually has). In the worst case, this could lead to scheduling starvation of other entities, as the reweighted entity would be considered highly eligible by the EEVDF scheduler.

In the kernels where both commits are present (v6.13 through v6.15), the bug has no observable runtime scheduling impact. The `se->vlag == 0` condition always prevents entry into the PLACE_LAG block for the pathological single-entity case, and for multi-entity cases, `nr_queued` being off by 1 does not affect the computation since it is only used as a boolean (non-zero) check. The `load` value for the lag inflation is derived from `cfs_rq->avg_load` and the current entity's weight, which are computed correctly regardless of `nr_queued`. However, the stale `nr_queued` represents a semantic incorrectness that could be exposed by future code changes that inspect `nr_queued` during `place_entity()`.

## Fix Summary

The fix in commit `c70fc32f` makes two changes to `reweight_entity()`:

1. **Adds `cfs_rq->nr_queued--` before the dequeue/reweight logic and `cfs_rq->nr_queued++` after the enqueue/placement logic.** This ensures that when `place_entity()` is called, `cfs_rq->nr_queued` accurately reflects the number of entities in the queue *excluding* the entity being re-placed. For a single entity, `nr_queued` drops to 0, correctly causing `place_entity()` to skip the PLACE_LAG block via the `cfs_rq->nr_queued` condition, regardless of whether the `se->vlag` optimization is present. The fixed code:

```c
if (se->on_rq) {
    update_curr(cfs_rq);
    update_entity_lag(cfs_rq, se);
    se->deadline -= se->vruntime;
    se->rel_deadline = 1;
    cfs_rq->nr_queued--;          // <-- FIX: decrement before placement
    if (!curr)
        __dequeue_entity(cfs_rq, se);
    update_load_sub(&cfs_rq->load, se->load.weight);
}
// ... weight scaling ...
if (se->on_rq) {
    place_entity(cfs_rq, se, 0);  // <-- now sees correct nr_queued
    update_load_add(&cfs_rq->load, se->load.weight);  // <-- moved after place
    if (!curr)
        __enqueue_entity(cfs_rq, se);
    cfs_rq->nr_queued++;          // <-- FIX: increment after placement
    update_min_vruntime(cfs_rq);
}
```

2. **Moves `update_load_add()` after `place_entity()`.** This is declared non-functional by the author since `place_entity()` does not use `cfs_rq->load` (it uses `cfs_rq->avg_load` from the augmented RB-tree). However, this ordering is consistent with other `place_entity()` callers in `enqueue_entity()`, where the entity's load is added to the cfs_rq *after* placement. This improves code consistency and maintainability.

The fix is correct and complete: it makes `reweight_entity()` adhere to the same `nr_queued` invariants as `enqueue_entity()`/`dequeue_entity()`, ensuring `place_entity()` always sees a consistent queue state. The `WARN_ON_ONCE(!load)` in `place_entity()` will no longer fire in any scenario.

## Triggering Conditions

The bug requires the following conditions:

1. **CONFIG_FAIR_GROUP_SCHED must be enabled.** The primary path to `reweight_entity()` with `se->on_rq == true` is through `update_cfs_group()`, which is only compiled when `CONFIG_FAIR_GROUP_SCHED` is set. The `set_user_nice()` path dequeues the task before calling `set_load_weight()` → `reweight_task()` → `reweight_entity()`, so `se->on_rq` is false in that path.

2. **A cgroup weight change must occur while the group SE is on the runqueue.** The trigger is `kstep_cgroup_set_weight()` (or writing to `cpu.weight`), which calls `sched_group_set_shares()` → `update_tg_cfs_group()` → `update_cfs_group()` → `reweight_entity()`. The group SE must be `on_rq` (i.e., the task group must have runnable tasks on the CPU in question).

3. **The group SE must be the sole entity on its parent cfs_rq.** This is required for the `WARN_ON_ONCE` to fire (if the `se->vlag` optimization is absent). With a single entity, `avg_load = 0` after dequeue, causing the `!load` condition. If there are multiple entities, `avg_load` reflects the remaining entities' weights and is non-zero.

4. **The `se->vlag` optimization (commit `4423af84b297`) must be absent.** In all kernel releases where `6d71a9c61604` exists (v6.13+), `4423af84b297` is also present (it was merged as an ancestor). The WARN is therefore unreachable in stock kernels. The bug becomes observable only if `4423af84b297` is reverted, or if a stable backport includes `6d71a9c61604` without `4423af84b297`.

5. **Topology: at least 2 CPUs.** CPU 0 is reserved for the kSTEP driver. The cgroup task must run on CPU 1 (or another non-zero CPU).

The bug is deterministic once the conditions are met: every cgroup weight change on a single-entity cfs_rq will produce the incorrect `nr_queued` value during `place_entity()`. However, the observable effect (WARN or scheduling anomaly) depends on whether the masking optimization is present.

## Reproduce Strategy (kSTEP)

The reproduction of this bug in kSTEP requires demonstrating the `nr_queued` inconsistency during `reweight_entity()`. Because the bug's visible effect (the `WARN_ON_ONCE`) is masked by the co-existing `se->vlag` optimization in all affected kernel versions, a direct observation of the scheduling anomaly requires one of two approaches: (a) adding a minor kSTEP callback to intercept the intermediate state, or (b) using kernel module tracing facilities (kprobes) from within the driver.

### Approach A: kSTEP Extension with Reweight Callback (Recommended)

Add a new callback `on_reweight_entity` to the `kstep_driver` struct, invoked inside `reweight_entity()` after `update_entity_lag()` and before `place_entity()`. The callback receives the `cfs_rq` and `se` pointers. This is a minor extension (adding a single callback hook in `kernel/sched/fair.c`).

**Driver setup:**
1. Create one task (`task_a`) pinned to CPU 1.
2. Create a cgroup `g0` and move `task_a` into it.
3. Wake `task_a` and tick several times to establish steady state.
4. At this point, `task_a` is the sole task on CPU 1 in cgroup `g0`. The group SE for `g0` on CPU 1's root cfs_rq may or may not be the sole entity (depends on whether other system tasks are on CPU 1). To guarantee it's the sole entity, pin all other tasks away from CPU 1.

**Trigger:**
1. Call `kstep_cgroup_set_weight("g0", new_weight)` to trigger `reweight_entity()` on the group SE.

**Detection (in the callback):**
1. Read `cfs_rq->nr_queued` inside the `on_reweight_entity` callback.
2. On the **buggy** kernel: `nr_queued` will be 1 (the group SE is counted but conceptually dequeued).
3. On the **fixed** kernel: `nr_queued` will be 0 (correctly decremented before the callback fires).
4. Report `kstep_fail()` if `nr_queued != 0` when the entity is the sole one on the cfs_rq.

### Approach B: Kprobe-Based Detection (No kSTEP Extension Needed)

Since kSTEP drivers are kernel modules, they can register kprobes. Register a kprobe on `reweight_entity` (which is a non-static function) or use `KSYM_IMPORT(reweight_entity)` to get its address. Within the kprobe pre-handler:

1. Check if `cfs_rq->nr_queued` is non-zero at entry.
2. After `place_entity` returns (using a kretprobe or a carefully placed jprobe), check `nr_queued` again.
3. On buggy kernel: nr_queued stays at its original value throughout.
4. On fixed kernel: nr_queued is decremented by 1 before place_entity and incremented after.

However, `reweight_entity` is a static function in some kernel versions, making kprobes harder to set up. Use `kallsyms_lookup_name()` or `KSYM_IMPORT` to resolve its address.

### Approach C: Indirect Detection via nr_queued Read (Simplest, No Extension)

A simpler but less precise approach:

1. Create task `task_a`, pin to CPU 1, move to cgroup `g0`.
2. Ensure `task_a` is the only runnable entity on CPU 1.
3. Read `cfs_rq->nr_queued` on CPU 1's root cfs_rq before the weight change: should be 1.
4. Call `kstep_cgroup_set_weight("g0", new_weight)`.
5. Read `cfs_rq->nr_queued` after: should be 1.
6. The intermediate stale value is not directly observable with this approach, but we can verify correctness by also reading `se->vruntime`, `se->deadline`, and `se->vlag` before and after the weight change and comparing them to expected values.

On both buggy and fixed kernels, the after-state values will be identical (since `se->vlag == 0` masks the bug). This approach can verify the reweight mechanics work but cannot distinguish buggy from fixed.

### Recommended Implementation Plan

The most viable approach combines Approach A with a minimal kSTEP extension:

1. **kSTEP extension**: Add a single line in `reweight_entity()` in the kernel's `fair.c` (via a small patch applied during `checkout_linux.py`): invoke a callback at the point between `update_entity_lag()` and `place_entity()`. Alternatively, use `static_call` or a tracepoint.

2. **Driver code (`eevdf_reweight_nr_queued_stale.c`)**:
   - Guard with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)`.
   - Create 1 task, pin to CPU 1, move to cgroup `g0`, wake it.
   - Tick 10 times to establish steady state.
   - Read `cfs_rq->nr_queued` for the parent cfs_rq of the group SE.
   - In the `on_reweight_entity` callback: assert `cfs_rq->nr_queued == 0` for a single-entity cfs_rq.
   - Trigger weight change: `kstep_cgroup_set_weight("g0", 500)`.
   - On buggy kernel: callback observes `nr_queued == 1` → `kstep_fail()`.
   - On fixed kernel: callback observes `nr_queued == 0` → `kstep_pass()`.

3. **Expected results**:
   - Buggy kernel (`c70fc32f~1`): FAIL — `nr_queued` is 1 during placement (stale).
   - Fixed kernel (`c70fc32f`): PASS — `nr_queued` is 0 during placement (correct).

If the callback extension is not desired, an alternative is to check dmesg for the `WARN_ON_ONCE` output after applying a targeted kernel patch that removes the `se->vlag` check from the PLACE_LAG condition. This more invasive approach directly triggers the visible WARN but requires modifying the kernel source under test.
