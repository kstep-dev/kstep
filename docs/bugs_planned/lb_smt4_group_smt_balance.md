# LB: SMT4 group_smt_balance Incorrect Busiest Selection and Imbalance Rounding

**Commit:** `450e749707bc1755f22b505d9cd942d4869dc535`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.6-rc2
**Buggy since:** v6.6-rc1 (introduced by `fee1759e4f04` "sched/fair: Determine active load balance for SMT sched groups")

## Bug Description

The `group_smt_balance` group type was introduced in commit `fee1759e4f04` to handle load balancing for SMT scheduling groups that are fully busy with sibling contention. The intent was to migrate tasks off an SMT core where multiple tasks share CPU capacity to an idle core elsewhere. However, the original implementation only considered SMT2 (2 threads per core) systems and did not correctly handle SMT4 systems like IBM POWER10, which have 4 hardware threads per core.

On SMT4 systems, any sched group with more than 2 running tasks gets classified as `group_smt_balance` (since `sum_h_nr_running > 1` triggers the `smt_balance()` check, and with 4 threads per core, even 3 tasks on a core leave idle CPUs). The problem is twofold: (1) the `update_sd_pick_busiest()` function used `group_fully_busy` comparison logic (based on `avg_load`) for `group_smt_balance` groups, which is incorrect when there are idle CPUs in the SMT group, and (2) the `sibling_imbalance()` function could compute an imbalance of 1 due to integer rounding when the local group was completely idle and the busiest group had tasks, preventing task migration when it should occur.

The `group_smt_balance` case in `update_sd_pick_busiest()` fell through directly into `group_fully_busy`, which selects the busiest group based on `avg_load`. But `avg_load` is not computed for groups below `group_overloaded` (it is always 0), so the first group encountered would always be selected as busiest. For SMT4 systems where `group_smt_balance` groups can still have idle CPUs, the correct behavior should be the same as `group_has_spare`: select the group with the fewest idle CPUs and the most running tasks. This ensures that the truly busiest SMT group (the one with the least spare capacity) is chosen for migration.

## Root Cause

There are two distinct bugs fixed by this commit, both affecting SMT4 (and higher SMT count) systems:

**Bug 1: Incorrect busiest group selection for `group_smt_balance`.**

In `update_sd_pick_busiest()`, the original code had:

```c
case group_smt_balance:
case group_fully_busy:
    // Select based on avg_load...
    if (sgs->avg_load < busiest->avg_load)
        return false;
    // avg_load is always 0 for non-overloaded groups, so first group wins
    ...
    break;
```

The `group_smt_balance` case simply fell through to `group_fully_busy`, which compares groups by `avg_load`. However, `avg_load` is only computed for `group_overloaded` groups — for all other group types it remains 0. This means when comparing two `group_smt_balance` groups, the first one encountered during the sched group iteration would always be selected as busiest (since `avg_load == avg_load == 0`, and the existing busiest is kept when loads are equal for SMT groups).

On SMT4 systems, a `group_smt_balance` group can have idle CPUs (e.g., an SMT4 core with 3 tasks has 1 idle CPU). In this scenario, the comparison should use the `group_has_spare` logic: prefer the group with fewer idle CPUs (more loaded). By using `avg_load` (which is always 0), the load balancer could pick a less-loaded group as busiest, leading to suboptimal task migration or failure to balance properly.

**Bug 2: Rounding error in `sibling_imbalance()` preventing migration.**

The `sibling_imbalance()` function computes how many tasks should be migrated between groups with different core counts. The normalization formula is:

```c
imbalance = 2 * imbalance + ncores_local + ncores_busiest;
imbalance /= ncores_local + ncores_busiest;
```

This adds `(ncores_local + ncores_busiest)` for rounding purposes. However, when the local group is fully idle (`local->sum_nr_running == 0`) and the busiest group has a small number of tasks, the rounding can produce an imbalance of exactly 1. The original guard only checked `imbalance == 0`:

```c
if (imbalance == 0 && local->sum_nr_running == 0 &&
    busiest->sum_nr_running > 1)
    imbalance = 2;
```

An imbalance of 1 is problematic because `calculate_imbalance()` for `group_smt_balance` sets `env->imbalance = 1`, and the subsequent `find_busiest_group()` check `return env->imbalance ? sds.busiest : NULL` expects the imbalance computed by `sibling_imbalance()` to be at least 2 (since `env->imbalance` is divided by 2 later in the `migrate_task` path for computing the actual number of tasks to move). With an imbalance of 1 that gets rounded down, no task is actually moved, leaving the local group completely idle while the busiest group has excess tasks.

Consider a concrete example on an SMT4 POWER10 system with the MC domain containing 4 SMT groups of 4 threads each. If `sg1` has 4 tasks (all CPUs busy), `sg2` has 0 tasks (all idle), and both groups have `ncores = 1` (since each is a single SMT4 core), then `ncores_busiest == ncores_local == 1`, so the equal-cores fast path is taken: `imbalance = 4 - 0 = 4`. This case works fine. But with asymmetric core counts (e.g., groups spanning different numbers of cores due to cluster topology), the rounding formula can produce imbalance = 1 when it should be 2.

## Consequence

The observable impact of these bugs is **suboptimal task distribution on SMT4 systems**, resulting in degraded performance. Specifically:

1. **Wrong busiest group selection**: On a system with multiple SMT4 sched groups at the MC domain level, the load balancer may select a less-loaded SMT group as the busiest instead of the most loaded one. For example, if group A has 3 tasks (1 idle CPU) and group B has 4 tasks (0 idle CPUs), the buggy code might keep group A as busiest if it was encountered first, rather than correctly preferring group B. This leads to tasks being pulled from the wrong source, leaving the actually-busiest group overcontended while other groups remain unbalanced.

2. **Failed task migration due to imbalance rounding**: When the local group is completely idle and could benefit from pulling tasks, the computed imbalance of 1 (instead of the needed 2) means the load balancer either migrates zero tasks or only one task in a situation where it should migrate more. This results in idle CPUs that could be productive remaining unused, reducing overall system throughput.

3. **Performance degradation on IBM POWER10 and similar SMT4 platforms**: These platforms were the primary systems affected. Shrikanth Hegde from IBM identified the issue during testing on POWER10 with SMT4 topology `[0 2 4 6][1 3 5 7][8 10 12 14][9 11 13 15]` at the MC domain. The bug caused measurable throughput loss in multi-threaded workloads because tasks were not being spread optimally across available cores.

There are no crashes, panics, or data corruption — the bug manifests purely as a performance regression (incorrect scheduling decisions) on SMT4+ architectures introduced by the v6.6-rc1 merge window.

## Fix Summary

The fix addresses both bugs with targeted changes to `update_sd_pick_busiest()` and `sibling_imbalance()`:

**Fix 1: Correct busiest selection for `group_smt_balance` with idle CPUs.**

In `update_sd_pick_busiest()`, the `group_smt_balance` case is modified to check whether either the candidate or the current busiest group has idle CPUs. If so, it jumps to the `has_spare` label to use the `group_has_spare` comparison logic (prefer the group with fewer idle CPUs; on tie, prefer more running tasks). If neither group has idle CPUs (both are fully loaded), it falls through to `group_fully_busy` as before:

```c
case group_smt_balance:
    if (sgs->idle_cpus != 0 || busiest->idle_cpus != 0)
        goto has_spare;
    fallthrough;
case group_fully_busy:
    ...
```

The `has_spare` label is placed just before the idle_cpus comparison in the `group_has_spare` case, after the `smt_vs_nonsmt_groups()` check. This reuses the existing spare-capacity logic without code duplication, which was an improvement suggested by Peter Zijlstra over the initial patch from Shrikanth Hegde that had separate comparison logic.

**Fix 2: Handle rounding to imbalance of 1 in `sibling_imbalance()`.**

The condition for overriding a zero imbalance is relaxed from `imbalance == 0` to `imbalance <= 1`:

```c
if (imbalance <= 1 && local->sum_nr_running == 0 &&
    busiest->sum_nr_running > 1)
    imbalance = 2;
```

When the local group is completely idle and the busiest group has more than one task, an imbalance of 1 is insufficient to trigger meaningful migration (it can round down to zero tasks). Setting it to 2 ensures at least one task will be migrated. This is correct because: if the local group has zero tasks and the busiest has more than one, there is clearly an imbalance worth acting on. The value 2 is used rather than 1 because the `migrate_task` path interprets the imbalance as task count × 2 (due to the normalize-and-round calculation used elsewhere).

## Triggering Conditions

To trigger this bug, the following conditions must be met:

- **SMT4 (or higher) topology**: The system must have scheduling groups with 4 or more SMT threads per core (e.g., IBM POWER10). On SMT2 systems, a `group_smt_balance` group always has 0 idle CPUs (both threads busy, since `sum_h_nr_running > 1` means at least 2 tasks on 2 CPUs), so the `idle_cpus` check in the fix is never triggered. SMT4 allows `group_smt_balance` with idle CPUs (e.g., 3 tasks on 4 threads = 1 idle CPU).

- **Multiple SMT sched groups at the MC (or higher) domain**: There must be at least 2 SMT scheduling groups being compared during load balancing. The bug manifests in `update_sd_pick_busiest()` when comparing two `group_smt_balance` groups.

- **Asymmetric task loading across SMT groups**: At least two SMT groups must have different numbers of tasks, with both having `sum_h_nr_running > 1` (so both are classified as `group_smt_balance`). For example, one group with 4 tasks and another with 2 tasks, or one with 3 tasks and another with 3 tasks but different idle CPU counts.

- **Idle destination CPU**: The CPU triggering load balancing (`env->idle != CPU_NOT_IDLE`) must be idle — the `smt_balance()` function returns false if the CPU is not idle.

- **For the rounding bug**: Groups with different `cores` counts (asymmetric groups, as seen in hybrid CPU topologies), or any configuration where the `sibling_imbalance()` normalization formula yields exactly 1. The local group must have zero running tasks (`sum_nr_running == 0`) and the busiest must have more than 1 task.

The bug is deterministic once the load balancing is triggered with the right task distribution. It does not require specific timing or race conditions — it is a logic error that occurs every time `update_sd_pick_busiest()` processes `group_smt_balance` groups with idle CPUs on SMT4 systems.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP by setting up an SMT4 topology and creating an imbalanced task distribution across SMT groups. Here is a concrete step-by-step plan:

### 1. Topology Setup

Configure QEMU with at least 8 CPUs. Set up SMT4 topology with 2 SMT groups of 4 threads each, forming one MC domain:

```c
kstep_topo_init();
// SMT groups: [1,2,3,4] and [5,6,7,8], CPU 0 reserved for driver
const char *smt[] = {"0", "1-4", "1-4", "1-4", "1-4", "5-8", "5-8", "5-8", "5-8"};
const char *mc[]  = {"0", "1-8", "1-8", "1-8", "1-8", "1-8", "1-8", "1-8", "1-8"};
kstep_topo_set_smt(smt, 9);
kstep_topo_set_mc(mc, 9);
kstep_topo_apply();
```

This creates two SMT4 sched groups `[1,2,3,4]` and `[5,6,7,8]` at the MC domain level. Both groups will have `SD_SHARE_CPUCAPACITY` set and `cores = 1`.

### 2. Task Creation and Pinning

Create tasks and pin them to create an asymmetric load:

- Pin 3 or 4 tasks to SMT group 1 (CPUs 1-4) — this group becomes `group_smt_balance` (or `group_overloaded` with 4+ tasks, so use exactly 3 tasks to get `group_smt_balance` with 1 idle CPU).
- Pin 2 tasks to SMT group 2 (CPUs 5-8) — this group also becomes `group_smt_balance` with 2 idle CPUs.
- Leave the local CPU (e.g., one of CPUs 5-8) idle to trigger idle load balancing.

```c
// Create 3 tasks on group 1 (CPUs 1-4): group_smt_balance with idle_cpus=1
struct task_struct *t1 = kstep_task_create();
struct task_struct *t2 = kstep_task_create();
struct task_struct *t3 = kstep_task_create();
kstep_task_pin(t1, 1, 4);
kstep_task_pin(t2, 1, 4);
kstep_task_pin(t3, 1, 4);

// Create 3 tasks on group 2 (CPUs 5-8): group_smt_balance with idle_cpus=1
struct task_struct *t4 = kstep_task_create();
struct task_struct *t5 = kstep_task_create();
struct task_struct *t6 = kstep_task_create();
kstep_task_pin(t4, 5, 8);
kstep_task_pin(t5, 5, 8);
kstep_task_pin(t6, 5, 8);
```

Alternatively, to test the busiest selection bug more directly: put 4 tasks on group 1 and 3 tasks on group 2. Both are `group_smt_balance`. Group 1 has `idle_cpus=0`, group 2 has `idle_cpus=1`. On buggy kernel, `update_sd_pick_busiest()` will use `avg_load` comparison (both 0), so whichever group is encountered first wins as busiest. On fixed kernel, the `has_spare` logic kicks in and correctly selects group 1 (fewer idle CPUs) as busiest.

### 3. Triggering Load Balance

Use `kstep_tick_repeat()` to advance time and trigger the load balancer. The idle CPU will attempt `load_balance()` in idle balancing:

```c
kstep_tick_repeat(20);  // Allow multiple load balancing intervals
```

### 4. Observing with Callbacks

Use `on_sched_balance_begin` and/or `on_sched_balance_selected` callbacks to monitor which group is selected as busiest and what the computed imbalance is. Import internal symbols to inspect the load balancer state:

```c
KSYM_IMPORT(sd_lb_stats);  // If needed
```

In `on_sched_balance_selected`, log:
- Which CPU triggered the balance
- The busiest group's `idle_cpus` and `sum_nr_running`
- The computed `env->imbalance`

### 5. Detection Criteria

**For Bug 1 (wrong busiest selection):**
- Set up group 1 with 4 tasks (idle_cpus=0) and group 2 with 3 tasks (idle_cpus=1).
- On the buggy kernel, if group 2 is encountered first during sched_group iteration, it will be kept as busiest even though group 1 has fewer idle CPUs. Check which group is selected as busiest by comparing the busiest group's idle_cpus against expectations.
- On the fixed kernel, group 1 (idle_cpus=0) should always be selected as busiest when compared to group 2 (idle_cpus=1).

**For Bug 2 (sibling_imbalance rounding):**
- This requires asymmetric core counts between groups, which is harder to set up with pure SMT4 topology. To test this path specifically, a hybrid topology with different core counts per group would be needed (e.g., one group with 2 cores and another with 1 core, using cluster scheduling). However, the `imbalance <= 1` fix also helps when `ncores_busiest == ncores_local` via the early return path, so it may be sufficient to verify that when local has 0 tasks and busiest has >1 tasks, at least 2 tasks of imbalance are computed.

### 6. Pass/Fail Criteria

- **Pass (fixed kernel)**: The busiest group selected by `update_sd_pick_busiest()` is the one with the fewest idle CPUs (most loaded). The imbalance computed by `sibling_imbalance()` is ≥ 2 when the local group is empty and the busiest has tasks.
- **Fail (buggy kernel)**: The busiest group is selected incorrectly (a group with more idle CPUs is chosen), or the imbalance is 0 or 1 when migration should occur.

### 7. Expected Behavior

On the **buggy kernel**: When two `group_smt_balance` groups with different idle_cpus counts are compared, the first one encountered may be kept as busiest regardless of which has fewer idle CPUs (since avg_load is 0 for both). Task migration may be suboptimal or not occur.

On the **fixed kernel**: The group with fewer idle CPUs is correctly selected as busiest (using `group_has_spare` comparison logic). When the local group is empty and busiest has tasks, sibling_imbalance correctly returns ≥ 2, ensuring task migration proceeds.

### 8. kSTEP Considerations

kSTEP can fully support this reproduction:
- **Topology**: `kstep_topo_set_smt()` can configure SMT4 groups. The topology setup creates the proper `SD_SHARE_CPUCAPACITY` domain flags needed for `smt_balance()` to classify groups as `group_smt_balance`.
- **Task management**: `kstep_task_create()` and `kstep_task_pin()` can place specific numbers of tasks on specific CPU ranges.
- **Observation**: `kstep_output_balance()` and `on_sched_balance_selected` callback can capture the load balancer's decisions. `KSYM_IMPORT()` can access internal variables if needed.
- **No framework changes needed**: All required APIs already exist in kSTEP.
