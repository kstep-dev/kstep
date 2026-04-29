# Load Balancing: Wrong Condition for avg_load Calculation in Wakeup Path

**Commit:** `6c8116c914b65be5e4d6f66d69c8142eb0648c22`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.7-rc1
**Buggy since:** v5.5-rc1 (introduced by commit `57abff067a08` "sched/fair: Rework find_idlest_group()")

## Bug Description

In the slow wakeup path of the CFS scheduler, the function `update_sg_wakeup_stats()` computes per-scheduling-group statistics used by `find_idlest_group()` to select the least loaded group for task placement. One of these statistics is `avg_load`, which normalizes the group's total load by its capacity. The code comment correctly states that computing `avg_load` makes sense only when the group is "fully busy" or "overloaded", since those are the only group types where `avg_load` is used as the comparison criterion in `update_pick_idlest()`.

However, the conditional check guarding the `avg_load` computation was inverted. The buggy code read `if (sgs->group_type < group_fully_busy)`, which evaluates to true only when `group_type == group_has_spare` (value 0), since `group_fully_busy` has value 1 in the `enum group_type`. This means `avg_load` was computed for groups with spare capacity (where it is never used) and was *not* computed for fully busy or overloaded groups (where it is the primary comparison metric).

This bug was introduced in v5.5-rc1 by commit `57abff067a08` ("sched/fair: Rework find_idlest_group()"), which rewrote the `find_idlest_group()` function to classify scheduling groups using the same `group_type` taxonomy as the load balancer. When writing `update_sg_wakeup_stats()`, the condition for `avg_load` computation was incorrectly coded with `<` instead of `==` or `>=`, resulting in the inverted logic.

## Root Cause

The `enum group_type` is ordered from least loaded to most loaded:

```c
enum group_type {
    group_has_spare = 0,
    group_fully_busy,     // 1
    group_misfit_task,    // 2
    group_asym_packing,   // 3
    group_imbalanced,     // 4
    group_overloaded      // 5
};
```

In `update_sg_wakeup_stats()` (line 8634 of the buggy kernel's `fair.c`), the code was:

```c
if (sgs->group_type < group_fully_busy)
    sgs->avg_load = (sgs->group_load * SCHED_CAPACITY_SCALE) /
                    sgs->group_capacity;
```

The condition `sgs->group_type < group_fully_busy` is equivalent to `sgs->group_type < 1`, which is only true when `sgs->group_type == group_has_spare` (0). This is the exact opposite of the intended behavior described in the comment directly above it.

The `avg_load` field is only used in `update_pick_idlest()` for the `group_overloaded` and `group_fully_busy` cases:

```c
case group_overloaded:
case group_fully_busy:
    /* Select the group with lowest avg_load. */
    if (idlest_sgs->avg_load <= sgs->avg_load)
        return false;
    break;
```

Because `avg_load` is never computed for these group types (it remains 0 from the `memset(sgs, 0, sizeof(*sgs))` at the start of `update_sg_wakeup_stats()`), the comparison `idlest_sgs->avg_load <= sgs->avg_load` always evaluates as `0 <= 0` → true, meaning the function always returns false. This means the first non-local fully_busy or overloaded group encountered in the iteration is always selected as the "idlest", regardless of actual load differences between groups.

Interestingly, the analogous function for the load balancer path, `update_sg_lb_stats()`, had the correct condition: it computed `avg_load` only when `sgs->group_type == group_overloaded`. The wakeup path needed the additional `group_fully_busy` case because `update_pick_idlest()` uses `avg_load` for both overloaded and fully busy groups, unlike the load balancer's `update_sd_pick_busiest()`.

## Consequence

The practical impact of this bug is suboptimal task placement during the slow wakeup path when multiple scheduling groups are classified as `group_fully_busy` or `group_overloaded`. In these scenarios, the scheduler cannot distinguish between groups with different load levels and always selects the first non-local group encountered during the sched_group iteration. This effectively makes group selection arbitrary rather than load-aware for busy systems.

On systems with multiple sched domains and multiple groups per domain (e.g., multi-socket or multi-cluster systems), this can cause tasks to be placed on already heavily loaded groups instead of the least loaded one. This leads to load imbalance, increased scheduling latency, reduced throughput, and unfair CPU time distribution. The effect is most pronounced on large systems under heavy load where many groups reach the fully_busy or overloaded state simultaneously.

While this bug does not cause a crash, hang, or data corruption, it represents a significant performance regression for workloads that rely on the slow wakeup path for initial task placement. Workloads with frequent wakeups, many short-lived tasks, or high task creation rates would be particularly affected since they exercise the `find_idlest_group()` path more frequently.

## Fix Summary

The fix replaces the incorrect comparison `sgs->group_type < group_fully_busy` with an explicit check for the two group types that actually use `avg_load`:

```c
if (sgs->group_type == group_fully_busy ||
    sgs->group_type == group_overloaded)
    sgs->avg_load = (sgs->group_load * SCHED_CAPACITY_SCALE) /
                    sgs->group_capacity;
```

This change ensures that `avg_load` is computed exactly when it is needed — for groups classified as `group_fully_busy` or `group_overloaded` — and not for any other group type. The explicit enumeration of the two cases (rather than using a range comparison like `>= group_fully_busy`) is more robust because it does not depend on the numeric ordering of the enum values and precisely matches the cases in `update_pick_idlest()` where `avg_load` is consulted.

After this fix, `update_pick_idlest()` can properly differentiate between multiple fully_busy or overloaded groups based on their actual normalized load, selecting the group with the lowest `avg_load` as the idlest. This restores the intended load-aware task placement behavior in the slow wakeup path.

The fix is minimal and correct: it changes only the condition, does not alter the computation, and aligns the code with both the existing comment and the usage in `update_pick_idlest()`. It was reviewed by Vincent Guittot (the author of the original rework) and acked by Mel Gorman.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

1. **Multiple scheduling groups in a sched domain:** The system must have at least two scheduling groups at some sched domain level, so that `find_idlest_group()` has multiple candidates to compare. This requires a multi-core system with a topology that creates separate groups (e.g., multi-socket, multi-cluster, or multi-LLC systems).

2. **Groups classified as fully_busy or overloaded:** At least two non-local groups must reach the `group_fully_busy` or `group_overloaded` classification. `group_fully_busy` occurs when `!group_has_capacity()` returns true (i.e., `sum_nr_running >= group_weight` or load exceeds `capacity * imbalance_pct / 100`). `group_overloaded` requires `group_is_overloaded()` to return true (i.e., `sum_nr_running > sum_h_nr_running` — there are non-CFS tasks — or the load exceeds the capacity by the imbalance percentage).

3. **Different load levels between groups:** The two fully_busy or overloaded groups must have meaningfully different load values. If both have the same load, the bug has no observable effect since the selection would be the same either way.

4. **Task wakeup via the slow path:** A task must be woken up via the slow wakeup path that calls `find_idlest_group()`. This happens when `select_task_rq_fair()` enters the slow path (as opposed to the fast path which uses `wake_affine`). The slow path is used when the wake_affine heuristic decides not to use the previous or waking CPU.

5. **Kernel version v5.5 through v5.6:** The bug exists only in kernels from v5.5-rc1 (when commit `57abff067a08` was merged) through v5.6 (before v5.7-rc1 when the fix was merged).

The bug is deterministic — it always produces the wrong result when the conditions are met. There is no race condition or timing dependency involved; it is a simple logic error in a conditional check.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reason:

### 1. Why This Bug Cannot Be Reproduced with kSTEP

**The fix targets kernel v5.7-rc1, which is older than kSTEP's minimum supported version of v5.15.** The bug was introduced in v5.5-rc1 and fixed in v5.7-rc1, meaning it only existed in the v5.5 and v5.6 kernel series. kSTEP supports Linux v5.15 and newer only, so the buggy code does not exist in any kernel version that kSTEP can build and run.

### 2. What Would Need to Be Added to kSTEP

No framework changes are needed — the limitation is purely a kernel version constraint. If kSTEP's version support were extended to include v5.5–v5.6 kernels, the bug could potentially be reproduced by:

- Setting up a multi-group topology (e.g., 4 CPUs across 2 groups)
- Creating enough CFS tasks to make groups fully_busy or overloaded
- Waking up a task and observing which group it is placed in
- Comparing the selected group's actual load versus the load of other groups

### 3. Version Constraint

The fix is in v5.7-rc1. The bug exists only in v5.5-rc1 through v5.6.x. Both are well below kSTEP's minimum supported kernel version of v5.15.

### 4. Alternative Reproduction Methods

Outside of kSTEP, this bug could be reproduced on a v5.5 or v5.6 kernel by:

- **Hardware or VM setup:** Use a multi-socket or multi-cluster machine (or QEMU with `-smp sockets=2,cores=2` to create separate scheduling groups).
- **Workload:** Run enough CPU-intensive tasks per group to push groups into `group_fully_busy` or `group_overloaded` classification (at least one task per CPU in each group).
- **Observation:** Use tracepoints (`sched:sched_wakeup`, `sched:sched_migrate_task`) or add `printk` instrumentation in `update_sg_wakeup_stats()` and `update_pick_idlest()` to log `avg_load` values and the selected idlest group.
- **Verification:** Create an asymmetric load pattern (e.g., 4 tasks on group A, 2 tasks on group B, both groups fully_busy) and repeatedly wake a sleeping task. On the buggy kernel, the task will consistently be placed on whichever group comes first in the iteration order, regardless of load. On the fixed kernel, it will prefer the group with lower `avg_load`.
- **Scripting:** A simple test could use `taskset` to pin groups of `stress-ng` workers to specific CPU sets, then repeatedly fork/exec a lightweight program and measure on which CPUs it lands using `/proc/self/stat` or `sched_getcpu()`.
