# Fix picking of tasks for core scheduling with DL server

- **Commit:** c8a85394cfdb4696b4e2f8a0f3066a1c921af426
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** Deadline scheduling, core scheduling

## Bug Description

The DL server's pick_task was incorrectly calling CFS's pick_next_task_fair() instead of pick_task(). Core scheduling's pick_task should only evaluate/check CFS tasks across CPUs, not actually pick the next task to run, causing RB tree corruption in CFS. Additionally, the ->dl_server field wasn't being cleared in pick_task_fair(), so if a DL task set this field but never ran (e.g., another HT has a stop task), a subsequent direct CFS task pick would see stale ->dl_server state, triggering a KASAN error in set_next_entity().

## Root Cause

The code conflated two different pick operations: pick_task (for comparing tasks across CPUs in core scheduling) and pick_next_task (for actually selecting the next task to run). DL server used a single callback that called pick_next_task_fair, which performs CFS-specific operations that aren't safe when called from pick_task context. Additionally, the ->dl_server field had no lifecycle management and could persist after the DL server stopped servicing, leaving dangling state.

## Fix Summary

Split the single DL server pick callback into two distinct operations: pick_next for actual task selection and pick_task for evaluation. Added a "peek" parameter to __pick_next_task_dl() to route calls appropriately based on the operation. The fix also clears the ->dl_server field in pick_task_fair() to ensure no stale state persists when CFS tasks are picked directly.

## Triggering Conditions

The bug requires core scheduling enabled with hyperthreading and DL servers servicing CFS tasks. Two scenarios trigger issues: (1) During core scheduling's cross-CPU task comparison (pick_task), the DL server incorrectly calls pick_next_task_fair() instead of pick_task_fair(), causing CFS RB tree corruption through improper dequeue/enqueue operations. (2) A DL task sets ->dl_server on a CFS task but never runs (e.g., other hyperthread has stop task), leaving stale dl_server state that triggers KASAN errors in set_next_entity() when the CFS task is later picked directly without DL server involvement.

## Reproduce Strategy (kSTEP)

Setup core scheduling with 2+ CPUs configured as hyperthreads. Use `kstep_topo_set_smt()` to establish SMT pairs. Create CFS tasks via `kstep_task_create()` and enable DL server for CFS (if configurable). Pin competing tasks: create a stop-class task on one HT and CFS tasks on sibling HT to force core scheduling decisions. Use `kstep_tick_repeat()` to advance scheduling decisions. Monitor via `on_tick_begin()` callback to log task picks and check for RB tree corruption. Detect bugs by: (1) CFS corruption through scheduler debug output, (2) KASAN errors in logs when stale dl_server state persists. Check `task->dl_server` field and CFS RB tree consistency after core scheduling operations.
