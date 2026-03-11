# sched/fair: Fix lag clamp

- **Commit:** 6e3c0a4e1ad1e0455b7880fad02b3ee179f56c09
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

In mixed slice workloads where different entities have different slice durations, the lag clamping mechanism was excessively constraining the vruntime lag of entities. The issue occurred because the lag limit was computed using only the current entity's slice (`2*se->slice`) without considering the maximum slice duration of other entities in the run queue, causing unnecessary serialization and unfair scheduling.

## Root Cause

The original lag clamping implementation used `2*se->slice` to compute the clamp limit, which only accounts for the current entity's slice duration. In a mixed workload with entities having different slices, this fails to track the actual maximum slice present in the run queue. The fix implemented a TODO comment that was left in the code: "XXX could add max_slice to the augmented data to track this."

## Fix Summary

The fix implements proper max_slice tracking by adding `cfs_rq_max_slice()` function, similar to the existing `cfs_rq_min_slice()`, and updates the augmented red-black tree to maintain max_slice information across all entities. The lag clamp limit is now computed using the actual maximum slice from the run queue instead of just the current entity's slice, ensuring fair treatment in mixed slice workloads.

## Triggering Conditions

The bug manifests in EEVDF-scheduled CFS runqueues containing entities with significantly different slice durations. Specifically, the lag clamping mechanism in `update_entity_lag()` was using `2*se->slice` to compute clamp limits, which excessively constrains entities with smaller slices when larger-slice entities coexist on the same runqueue. The issue occurs during lag updates when tasks are dequeued/enqueued, causing unfair serialization. Mixed slice workloads can arise from different task nice levels, CPU controller weights, or explicitly set custom slices. The bug affects scheduler fairness by preventing proper lag accumulation for entities with smaller slices relative to the runqueue maximum.

## Reproduce Strategy (kSTEP)

Create a mixed slice workload with 2 CPUs (CPU 1 for tasks). Use `kstep_task_create()` to spawn multiple tasks with different nice levels (e.g., nice 0, nice 5, nice -5) via `kstep_task_set_prio()` to generate varying slice durations. Pin all tasks to CPU 1 using `kstep_task_pin()`. In `run()`, wake all tasks with `kstep_task_wakeup()`, then use `kstep_tick_repeat()` to let them accumulate different lag values. Periodically pause/wake tasks to trigger `update_entity_lag()` calls. Use `on_tick_begin` callback to log task vlag values and slice durations. Check if smaller-slice tasks have disproportionately clamped lag compared to the theoretical limit based on maximum slice in the runqueue. The bug manifests as excessive lag clamping for tasks with smaller slices, observable through restricted vlag values relative to the theoretical `calc_delta_fair(max_slice, se)` limit.
