# sched/eevdf: Fix vruntime adjustment on reweight

- **Commit:** eab03c23c2a162085b13200d7942fc5a00b5ccc8
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (EDF scheduling policy)

## Bug Description

When a task on the runqueue with non-zero lag is reweighted (priority changed), its vruntime is not properly adjusted, resulting in incorrect scheduling fairness. The entity maintains its old vruntime despite its weight changing, which breaks the invariant that lag should be preserved across reweight operations. This causes scheduling anomalies where fairness metrics become incorrect.

## Root Cause

The original code in `reweight_entity()` failed to adjust vruntime when an on-queue entity at !0-lag point (V ≠ v) was reweighted. The vruntime adjustment requires complex mathematical reasoning: when weight changes from w to w', the new vruntime must be `v' = V - (V - v) * w / w'` to preserve the lag invariant `lag = (V - v) * w`. Without this adjustment, the lag is lost and scheduling fairness deteriorates.

## Fix Summary

The fix introduces `reweight_eevdf()` function that implements the mathematically proven vruntime adjustment formula with complete proofs for COROLLARY #1 (vruntime must be adjusted at !0-lag) and COROLLARY #2 (reweight doesn't affect weighted average vruntime). It changes `reweight_entity()` to call this function for on-queue entities, replacing the incorrect `avg_vruntime_sub/add` operations with proper `__dequeue_entity/__enqueue_entity` calls that maintain tree consistency after vruntime changes.

## Triggering Conditions

The bug occurs when an on-queue task with non-zero lag is reweighted (priority changed). Specifically:
- A task is enqueued on the CFS runqueue with vruntime != avg_vruntime (creating !0-lag)
- The task remains on the runqueue (not currently running, so se->on_rq && cfs_rq->curr != se)
- Task priority is changed via nice() or setpriority() syscalls, triggering reweight_entity()
- Before the fix, vruntime was not adjusted, causing lag invariant violation
- This breaks scheduling fairness as the task's effective lag changes unintentionally

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (driver uses CPU 0). Create tasks with different weights to establish non-zero lag:
1. **Setup:** Create 3 tasks (A, B, C) with different priorities using `kstep_task_create()` and `kstep_task_set_prio()`
2. **Establish lag:** Pin tasks A,B to CPU 1, wake both, run several ticks with `kstep_tick_repeat()` to create vruntime differences
3. **Create !0-lag state:** Pause task B with `kstep_task_pause()` to save positive lag, then tick so A runs alone
4. **Wake with lag:** Use `kstep_task_wakeup()` on B to place it with non-zero lag on the runqueue
5. **Trigger bug:** Change B's priority with `kstep_task_set_prio()` while B is on-queue but not current
6. **Detection:** Use `on_tick_begin()` callback to log vruntime, avg_vruntime, and lag values before/after reweight
7. **Verification:** Check if lag invariant `(avg_vruntime - vruntime) * weight` is preserved across reweight
