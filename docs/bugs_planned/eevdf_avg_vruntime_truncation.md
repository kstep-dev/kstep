# EEVDF: avg_vruntime() Negative Division Rounds Wrong Direction

**Commit:** `650cad561cce04b62a8c8e0446b685ef171bc3bb`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.6-rc5
**Buggy since:** v6.6-rc1 (introduced by `af4cf40470c2` "sched/fair: Add cfs_rq::avg_vruntime")

## Bug Description

The EEVDF (Earliest Eligible Virtual Deadline First) scheduler uses the function `avg_vruntime()` to compute the weighted average virtual runtime (V) of all runnable entities on a CFS run queue. This value V is critical because it defines the eligibility boundary: an entity is eligible to run if and only if its vruntime ≤ V (equivalently, its lag ≥ 0). When a new task is placed on the run queue (via `place_entity()`), its vruntime is set based on `avg_vruntime()` so that the task starts at the average and is immediately eligible.

The bug is that `avg_vruntime()` uses `div_s64(avg, load)` to perform integer division, and integer division in C truncates toward zero (not toward negative infinity). When the numerator `avg` is non-negative, truncation toward zero is equivalent to floor, producing a result that is slightly left of (or exactly at) the true mathematical average. This leftward bias ensures that a task placed at the returned value satisfies the eligibility check `avg >= key * load`.

However, when `avg` is negative — which happens when the weighted-average key of all runnable entities is below `min_vruntime` — truncation toward zero becomes a ceiling operation, not a floor. The result is shifted slightly right of (above) the true average. A task placed at this ceiling value may fail the eligibility check, violating the fundamental invariant that placing a task at `avg_vruntime()` must yield an eligible entity.

The negative `avg` condition arises naturally when a task with large positive vlag (i.e., it received less service than it deserved) is woken up. The `place_entity()` function places such a task below `min_vruntime` using its preserved vlag, giving it a negative entity key. If this negative-key task has enough weight to pull the weighted sum `cfs_rq->avg_vruntime` below zero, the bug is triggered.

## Root Cause

The root cause lies in the mathematical properties of integer division applied to signed values. The `avg_vruntime()` function computes:

```c
u64 avg_vruntime(struct cfs_rq *cfs_rq)
{
    s64 avg = cfs_rq->avg_vruntime;
    long load = cfs_rq->avg_load;
    // ... include curr if on_rq ...
    if (load)
        avg = div_s64(avg, load);
    return cfs_rq->min_vruntime + avg;
}
```

The field `cfs_rq->avg_vruntime` stores `Σ(key_i * weight_i)` where `key_i = se->vruntime - cfs_rq->min_vruntime`. The field `cfs_rq->avg_load` stores `Σ(weight_i)`. The true weighted average is `Σ(key_i * weight_i) / Σ(weight_i)`, and the function returns `min_vruntime + avg/load`.

The eligibility check in `entity_eligible()` uses the non-divided form to avoid precision loss:

```c
int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    s64 avg = cfs_rq->avg_vruntime;
    long load = cfs_rq->avg_load;
    // ... include curr ...
    return avg >= entity_key(cfs_rq, se) * load;
}
```

For a task placed exactly at `avg_vruntime()`, its key is `avg_vruntime() - min_vruntime = div_s64(avg, load)`. So the eligibility test becomes: `avg >= div_s64(avg, load) * load`.

When `avg >= 0`: `div_s64(avg, load) = floor(avg/load)`. Since `floor(avg/load) * load ≤ avg`, the check passes. ✓

When `avg < 0`: `div_s64(avg, load) = ceil(avg/load)` (because C truncates toward zero for negative values, e.g., `div_s64(-7, 3) = -2`, not `-3`). Since `ceil(avg/load) * load ≥ avg` (the ceiling rounds the magnitude down, but in the negative direction this means the product overshoots), the check `avg >= ceil(avg/load) * load` can fail when there is any remainder. ✗

Concretely, suppose `avg = -7` and `load = 3`:
- `div_s64(-7, 3) = -2` (truncation toward zero, i.e., ceiling for negatives)
- Eligibility: `-7 >= -2 * 3` → `-7 >= -6` → **false** (bug!)
- Correct floor: `floor(-7/3) = -3`
- Eligibility: `-7 >= -3 * 3` → `-7 >= -9` → **true** (correct)

The fix adds `avg -= (load - 1)` before the division when `avg < 0`, converting the truncation-toward-zero into a floor operation for negative numbers. This is the standard technique: `floor(a/b) = (a - (b-1)) / b` when `a < 0` and `b > 0` in truncating integer division.

## Consequence

The immediate consequence is that a freshly placed task — one woken from sleep or newly forked — can be placed at a vruntime that makes it ineligible according to `entity_eligible()`. This violates the EEVDF scheduling invariant and has several downstream effects:

1. **Scheduling delay / starvation**: The `pick_eevdf()` function, which selects the next task to run, skips entities that fail the eligibility check. A task placed at the (incorrectly rounded) average will not be picked despite being at what should be the fair average position. It must wait for other tasks to advance `min_vruntime` past its position before becoming eligible again, causing unfair scheduling delay.

2. **Broken placement contract**: The `place_entity()` function explicitly uses `avg_vruntime()` as the base for placement with the assumption that vruntime = V makes a task eligible. When this assumption is violated, the entire lag-based placement mechanism in EEVDF produces incorrect results. Tasks with zero vlag (new tasks or tasks that received exactly their fair share) should be immediately eligible, but they are not.

3. **Cascading fairness violations**: Since `update_entity_lag()` also calls `avg_vruntime()` to compute `lag = V - se->vruntime`, the ceiling error propagates into lag computation. A slightly-too-high V produces slightly-too-large lag values for tasks below V and slightly-too-small lag values for tasks above V, subtly distorting the fairness tracking that EEVDF depends on.

The bug does not cause a kernel crash or panic, but manifests as subtle unfairness in task scheduling, with waking tasks occasionally experiencing unexpected delays before being selected to run.

## Fix Summary

The fix adds a three-line adjustment inside `avg_vruntime()` that ensures the division always produces a floor result regardless of the sign of the numerator:

```c
if (load) {
    /* sign flips effective floor / ceil */
    if (avg < 0)
        avg -= (load - 1);
    avg = div_s64(avg, load);
}
```

When `avg >= 0`, `div_s64` already floors, so no adjustment is needed. When `avg < 0`, subtracting `(load - 1)` before dividing converts the truncation-toward-zero into a floor. This works because for any `a < 0` and `b > 0`: `floor(a/b) = (a - b + 1) / b` using truncating division. The subtraction of `(load - 1)` shifts the value just enough that the truncation discards the correct amount.

After the fix, `avg_vruntime()` always returns a value with a "left bias" — it is at or slightly below the true weighted average. This guarantees that a task placed at the returned position satisfies `entity_eligible()`, restoring the invariant that the comment now explicitly documents: "avg_runtime() + 0 must result in entity_eligible() := true".

The fix is minimal and correct: it only modifies the rounding behavior for the negative case without affecting the positive case, and it uses a well-known integer arithmetic identity. The `load` variable is always positive (it is a sum of weights of runnable tasks), so `(load - 1)` is always non-negative, and the subtraction always increases the magnitude of the negative `avg`, which is what's needed to shift from ceiling to floor behavior.

## Triggering Conditions

The bug triggers when all of the following conditions hold simultaneously:

1. **Negative weighted-average key (`avg < 0`)**: The accumulated `cfs_rq->avg_vruntime` value (including the current task's contribution) must be negative. This means the load-weighted average of all entity keys `(vruntime - min_vruntime)` is below zero — i.e., the "center of mass" of all runnable entities is below `min_vruntime`.

2. **Non-zero remainder**: The magnitude of `avg` must not be evenly divisible by `load`. If `avg % load == 0`, then truncation and floor produce the same result, and the bug is not observable.

3. **Task placement at avg_vruntime()**: A task must be placed (via `place_entity()`) using the return value of `avg_vruntime()` while conditions 1 and 2 hold. This happens when a sleeping task is woken up or when a new task is forked.

**How to create negative avg**: The most reliable way is:
- Have two CFS tasks with different nice values (different weights) running on the same CPU.
- Let them accumulate vruntime, then pause the higher-weight task (lower nice value) while it has positive vlag (has received less service than its fair share).
- Wake the paused task after `min_vruntime` has advanced (while only the lower-weight task was running). Due to lag-based placement (`PLACE_LAG` feature), the task is placed at `V - inflated_vlag`, which can be below `min_vruntime`, giving it a negative entity key.
- If this negative-key task's contribution to `avg_vruntime` is large enough (due to its high weight), the total `avg` becomes negative.

**Configuration requirements**:
- EEVDF scheduler (CONFIG_SCHED_CLASS_CFS, which is always enabled; specifically the EEVDF code path from v6.6-rc1 onwards).
- `PLACE_LAG` sched feature enabled (it is by default).
- At least 2 CPUs (one for the driver on CPU 0, one for the test tasks).

**Reliability**: The bug is deterministic when the right state is constructed. It does not require a race condition; it only requires arranging for `avg < 0` with a non-zero remainder in the division.

## Reproduce Strategy (kSTEP)

The bug is fully reproducible with kSTEP. An existing driver (`kmod/drivers/avg_vruntime_ceil.c`) already demonstrates a working approach. The strategy below describes the complete reproduction plan:

### Step 1: Task Setup

Create three CFS tasks pinned to CPU 1 (CPU 0 is reserved for the driver):
- **Task A**: nice 1 (weight 820 after `scale_load_down`). Lower weight means its vruntime advances faster.
- **Task B**: nice 0 (weight 1024 after `scale_load_down`). Higher weight, slower vruntime advancement.
- **Task C**: A "probe" task, nice 0 (weight 1024), initially paused.

The different weights between A and B are essential: they ensure that `avg / load` has a non-zero remainder, making the truncation direction matter.

### Step 2: Build Negative avg State

1. Wake tasks A and B on CPU 1. Let them run for ~10 ticks to accumulate vruntime. Because A has lower weight, its vruntime advances faster, so B ends up with lower vruntime and positive vlag (B has received less than its fair share).
2. Wait until A is the current task (`cfs_rq->curr == &task_a->se`). This puts B on the RB tree with a positive lag.
3. Pause task B. The scheduler calls `update_entity_lag()` which saves B's positive vlag.
4. Let A run alone for ~10 more ticks. Since A is the only runnable task, `min_vruntime` advances with A's vruntime.
5. Wake task B. `place_entity()` computes `vruntime = avg_vruntime(cfs_rq) - inflated_vlag`. Because B had positive vlag, it is placed below `min_vruntime`, giving it a negative `entity_key`. With B's high weight (1024) multiplied by this negative key, `cfs_rq->avg_vruntime` is pulled negative.

### Step 3: Probe with Task C

Wake task C with zero vlag. `place_entity()` calls `avg_vruntime()` to determine C's vruntime. On the buggy kernel, `avg < 0` and `div_s64` rounds toward zero (ceiling), returning a value slightly too high. Task C is placed at this too-high position.

### Step 4: Verify Eligibility

After C is placed, use `kstep_eligible(&task_c->se)` to check whether C passes the eligibility test. Also manually read `cfs_rq->avg_vruntime`, `cfs_rq->avg_load`, and compute the invariant check `avg >= key_c * load`.

**On buggy kernel (v6.6-rc1 to v6.6-rc4)**: `kstep_eligible()` returns 0 (false) for task C — a task freshly placed at `avg_vruntime()` is not eligible. Report `kstep_fail()`.

**On fixed kernel (v6.6-rc5+)**: `kstep_eligible()` returns 1 (true) — the floor correction ensures C is eligible. Report `kstep_pass()`.

### Step 5: Diagnostic Logging

Log the following values at the verification point for debugging:
- `cfs_rq->avg_vruntime` (the raw weighted sum, should be negative)
- `cfs_rq->avg_load` (total weight of runnable entities)
- `task_b->se.vlag` (should be positive after pause)
- `task_b->se.vruntime` and its entity key (should be negative)
- `task_c->se.vruntime` and its entity key
- The return value of `avg_vruntime(cfs_rq)` (imported via `KSYM_IMPORT`)
- The result of `kstep_eligible(&task_c->se)`

### kSTEP API Usage

- `kstep_task_create()` × 3 for tasks A, B, C
- `kstep_task_set_prio(task_a, 1)` to set nice 1
- `kstep_task_wakeup()` / `kstep_task_pause()` for state transitions
- `kstep_tick_repeat(n)` to advance virtual time
- `kstep_eligible(&se)` for the eligibility check
- `KSYM_IMPORT(avg_vruntime)` to call `avg_vruntime()` directly
- `kstep_pass()` / `kstep_fail()` for result reporting
- Direct access to `cpu_rq(1)->cfs` internals via `internal.h`

### Version Guard

Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)` since the `avg_vruntime()` function and EEVDF placement were introduced in v6.6-rc1 via commit `af4cf40470c2`.

### No kSTEP Extensions Needed

This bug is fully reproducible with the existing kSTEP API. The `kstep_eligible()` function already provides the eligibility check, `KSYM_IMPORT` allows calling `avg_vruntime()` directly, and the task lifecycle APIs (`create`, `set_prio`, `wakeup`, `pause`) are sufficient to construct the required scheduler state. No framework modifications are necessary.
