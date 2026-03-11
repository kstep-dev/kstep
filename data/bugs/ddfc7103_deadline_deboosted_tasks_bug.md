# sched/deadline: Fix BUG_ON condition for deboosted tasks

- **Commit:** ddfc710395cccc61247348df9eb18ea50321cbed
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

Tasks being deboosted from SCHED_DEADLINE crash the kernel with a triggered BUG_ON condition when they enter enqueue_task_dl() one last time. The condition checks both that the task is NOT boosted (since they are being deboosted) and that flags equal ENQUEUE_REPLENISH exactly, but this check is logically contradictory and always triggers the BUG_ON.

## Root Cause

The BUG_ON condition is logically flawed: it resides in the else-if branch for non-boosted tasks, but it checks `!is_dl_boosted()` as part of the condition. Since the task enters this branch only when not boosted, the condition is always false and the BUG_ON always triggers. Additionally, the flags check `flags != ENQUEUE_REPLENISH` is overly strict, disallowing additional legitimate flags that may be set.

## Fix Summary

The fix replaces the erroneous BUG_ON with a conditional check that only validates the ENQUEUE_REPLENISH flag is present (using bitwise AND), while removing the contradictory boost status check. If the flag is missing, a deferred printk warning is issued instead of crashing the kernel, allowing the task to proceed with deboosting.

## Triggering Conditions

The bug occurs when a task that was temporarily boosted to SCHED_DEADLINE (via priority inheritance) is being deboosted back to its original scheduling class. The task enters `enqueue_task_dl()` one final time during deboosting with:
- Task is no longer boosted (`!is_dl_boosted(&p->dl)` is true)  
- Task's normal priority is not DEADLINE (`!dl_prio(p->normal_prio)` is true)
- Task enters the else-if branch for non-DEADLINE tasks being deboosted
- The BUG_ON condition `(!is_dl_boosted(&p->dl) || flags != ENQUEUE_REPLENISH)` always triggers since the first part is always true in this branch, making the entire OR expression true

Priority inheritance boosting typically occurs when a DEADLINE task blocks on a mutex held by a lower priority task, temporarily boosting the holder to DEADLINE priority.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs. Create a priority inheritance scenario where a DEADLINE task waits on a mutex held by a CFS task:
- In `setup()`: Create 2 tasks - one DEADLINE (`kstep_task_deadline()`) and one CFS 
- Use a shared resource (mutex/futex) that the CFS task acquires first
- Have the DEADLINE task attempt to acquire the same resource, causing priority inheritance to boost the CFS task to DEADLINE
- Release the mutex from the CFS task, triggering deboosting back to CFS
- Use `on_tick_begin()` callback to monitor task priority transitions and detect when `enqueue_task_dl()` is called during deboosting
- Check kernel logs for BUG_ON trigger or the warning message from the fix
- The bug manifests as a kernel panic on the buggy version, while the fixed version shows a deferred warning message
