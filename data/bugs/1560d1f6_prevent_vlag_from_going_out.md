# sched/eevdf: Prevent vlag from going out of bounds in reweight_eevdf()

- **Commit:** 1560d1f6eb6b398bddd80c16676776c0325fe5fe
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS/EEVDF

## Bug Description

The `reweight_eevdf()` function could overflow when computing `vlag` (virtual lag) due to the scaling multiplication with entity weight (20-bit fixed point). This integer overflow would cause the calculated vruntime to become incorrect, leading to `entity_eligible()` falsely returning false for all entities. As a result, `pick_eevdf()` would return NULL, triggering a NULL pointer dereference crash. The bug was rare but fatal when it occurred.

## Root Cause

The `reweight_eevdf()` function directly computed `vlag = (s64)(avruntime - se->vruntime)` without clamping it to the valid range, unlike `update_entity_lag()` which clamped vlag using a limit based on the entity's slice. When `vlag` was subsequently scaled by the old weight and divided by the new weight in the s64 multiplication, it could overflow, producing a wildly incorrect value for the new vruntime.

## Fix Summary

The fix extracts the vlag clamping logic from `update_entity_lag()` into a new helper function `entity_lag()` that computes and clamps vlag to the valid range [-limit, limit]. The `reweight_eevdf()` function is updated to use this helper, ensuring vlag is properly bounded before the weight scaling operation occurs.

## Triggering Conditions

The bug triggers during task weight changes (nice values/cgroups) when:
- A task has accumulated a very large positive or negative vlag value (avruntime - se->vruntime)
- The task undergoes reweighting via `reweight_eevdf()` (e.g., nice level change or cgroup weight update)
- The unclamped vlag multiplied by old weight exceeds s64 range during scaling calculation
- This causes integer overflow, producing incorrect vruntime that makes `entity_eligible()` return false for all entities
- Subsequent `pick_eevdf()` returns NULL, leading to NULL pointer dereference
- Most commonly occurs with tasks that have run for extended periods with different weights or have large vruntime differences

## Reproduce Strategy (kSTEP)

Setup requires 2+ CPUs. Create two tasks with different weights, run one extensively to accumulate large vlag, then trigger reweighting:
1. Use `kstep_task_create()` to create tasks A and B on CPU 1  
2. Set different nice values: `kstep_task_set_prio(task_a, -10)` and `kstep_task_set_prio(task_b, 10)`
3. Wake both tasks with `kstep_task_wakeup()`, run via `kstep_tick_repeat(500)` to accumulate large vruntime differences
4. Pause task B with `kstep_task_pause()` to save large vlag value  
5. Run task A alone via `kstep_tick_repeat(1000)` to advance min_vruntime significantly
6. Change task B's weight dramatically: `kstep_task_set_prio(task_b, -19)` 
7. Wake task B with `kstep_task_wakeup()` to trigger `reweight_eevdf()` overflow
8. Use `on_tick_end` callback to check if `pick_eevdf()` returns NULL or `entity_eligible()` fails
9. Log vlag values before/after reweight and check for vruntime corruption via TRACE_INFO
