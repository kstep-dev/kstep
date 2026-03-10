# sched/fair: Fix SMT4 group_smt_balance handling

- **Commit:** 450e749707bc1755f22b505d9cd942d4869dc535
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler) / Load Balancing

## Bug Description

On SMT4 systems, the load balancing logic for `group_smt_balance` sched groups was incorrectly handling CPU imbalance calculations. Two issues existed: (1) a rounding error in the `sibling_imbalance()` function where the condition checked only for `imbalance == 0` but failed to handle cases where the rounded result was `1`, causing the scheduler to not pull tasks when it should; and (2) improper handling of SMT groups with spare CPUs, treating them as fully busy instead of leveraging available capacity.

## Root Cause

The `sibling_imbalance()` function uses a normalization formula that divides `(2 * imbalance + ncores_local + ncores_busiest)` by the total number of cores. This division can produce a value of `1` due to rounding. The old check `imbalance == 0` missed this case, so when the local group was fully idle and the busy group had only slightly more load, the function incorrectly returned `0` or `1` instead of forcing a task migration. Additionally, the `group_smt_balance` case in `update_sd_pick_busiest()` did not distinguish between SMT groups with spare CPUs versus those that were fully busy, leading to incorrect busiest group selection and suboptimal task pulling decisions.

## Fix Summary

The fix changes the condition from `imbalance == 0` to `imbalance <= 1` to correctly handle the rounding effect, ensuring that when the local group is fully idle and the busy group has more than one task, imbalance is set to 2 to trigger at least one task pull. Additionally, it adds logic to the `group_smt_balance` case to check if either group has spare CPUs; if so, it falls through to `has_spare` handling to properly select the busiest group based on idle CPU count rather than treating all fully busy SMT groups identically.

## Triggering Conditions

The bug occurs during load balancing in SMT4 topologies with `group_smt_balance` sched groups. Key conditions:
- SMT4 system with at least 8 CPUs (2 SMT4 cores minimum for cross-group balancing)
- One SMT group completely idle (local group with `sum_nr_running == 0`)
- Another SMT group busy with 2+ tasks (to trigger `group_smt_balance` classification)
- Different core counts between local and busy groups causing rounding in `sibling_imbalance()`
- Load balancing triggered when `env->idle != CPU_NOT_IDLE` (idle or newly idle CPU)
- Normalized imbalance calculation yields exactly `1` due to division rounding
- The scheduler reaches `update_sd_pick_busiest()` with `group_smt_balance` groups

## Reproduce Strategy (kSTEP)

Setup SMT4 topology with 8+ CPUs using `kstep_topo_set_smt()` to create 4-way SMT groups.
Create 4+ tasks and use `kstep_task_pin()` to distribute them unevenly: pin 2+ tasks to one SMT group (CPUs 1-4) and leave another group (CPUs 5-8) completely idle. Run `kstep_tick_repeat()` to establish the imbalanced state.
In load balancing callbacks (`on_sched_balance_begin`), log `sum_nr_running` for each group and verify the busy group has `group_smt_balance` classification.
Use `kstep_task_wakeup()` on the idle group to trigger load balancing from a newly idle state.
Monitor `sibling_imbalance()` calculations via kernel logging to detect when normalized imbalance equals 1 but local group remains idle.
Check if task migration fails to occur when it should, indicating the bug where `imbalance == 0` check missed the `imbalance == 1` case.
Compare behavior with/without the fix to demonstrate incorrect load balancing decisions in SMT4 `group_smt_balance` scenarios.
