# Fix EEVDF entity placement bug causing scheduling lag

- **Commit:** 6d71a9c6160479899ee744d2c6d6602a191deb1f
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (EEVDF)

## Bug Description

The kernel's EEVDF scheduler incorrectly computes vruntime and deadline when reweighting an entity that is already on the runqueue, causing the entity's lag to accumulate across weight transitions instead of being preserved. This manifests as tracing output showing increasingly incorrect lag values that "shoot out further and further" with each reweight operation, causing scheduling errors and potential starvation.

## Root Cause

The `reweight_eevdf()` function computed the new vruntime using a complex formula that did not account for the entity placement logic implemented in `place_entity()`. While the lag calculation itself was correct, the subsequent vruntime computation from the lag ignored crucial adjustments that `place_entity()` applies when positioning entities in the scheduling tree, leading to a mismatch between the computed lag and the actual vruntime assigned.

## Fix Summary

The fix removes the problematic `reweight_eevdf()` function and simplifies `reweight_entity()` to reuse the existing `place_entity()` function for computing the new vruntime and deadline after a reweight. This ensures that the vruntime calculation is consistent with the entity placement logic and properly preserves lag across weight transitions.

## Triggering Conditions

This bug occurs during entity reweighting operations on tasks already on the runqueue, particularly prominent with DELAY_DEQUEUE enabled. The key conditions are:
- An EEVDF-scheduled entity with non-zero lag (vlag != 0) must be on the runqueue 
- The entity undergoes a weight change via `reweight_entity()` while remaining on_rq
- Multiple successive reweight operations amplify the error, causing lag to "shoot out further and further"
- The bug manifests in the `reweight_eevdf()` function's vruntime calculation that ignores `place_entity()` logic
- Trace output shows incorrect lag preservation across weight transitions (e.g., 1048576 -> 2 -> 1048576 weight cycles)

## Reproduce Strategy (kSTEP)

Reproduce with 2+ CPUs by creating entity reweight cycles that trigger the broken lag calculation:
- In `setup()`: Create two tasks with different weights using `kstep_task_create()` and `kstep_task_set_prio()`
- In `run()`: Pin tasks to CPU 1, wake them with `kstep_task_wakeup()`, run several ticks with `kstep_tick_repeat()`
- Pause one task to build up lag via `kstep_task_pause()`, then resume with `kstep_task_wakeup()`
- Trigger reweight cycles by repeatedly changing task priority while on runqueue using `kstep_task_set_prio()`
- Use `on_tick_begin()` callback to log entity vlag, vruntime values and detect incorrect lag accumulation
- Bug detected when lag values don't return to original after symmetric weight transitions (e.g., weight 1024->2->1024 should restore original lag)
