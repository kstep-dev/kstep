# sched: Add missing memory barrier in switch_mm_cid

- **Commit:** fe90f3967bdb3e13f133e5f44025e15f943a99c5
- **Affected file(s):** kernel/sched/sched.h
- **Subsystem:** core scheduler (mm_cid context tracking)

## Bug Description

Many architectures (e.g., arm64) do not have a memory barrier in their switch_mm() implementation. The scheduler's mm_cid (memory context ID) code assumed all architectures provided this barrier, which could lead to a race condition where sched_mm_cid_remote_clear() unsafely clears the actively used cid when transitioning from user space to user space. This violates the memory ordering guarantees required for correct mm_cid tracking.

## Root Cause

The switch_mm_cid() function in the scheduler relies on a memory barrier between storing to rq->curr and accessing the mm_cid fields. While some architectures provide this barrier in switch_mm() itself, others (e.g., arm64) only provide it later in finish_lock_switch() or switch_to(). The code assumed a barrier was always present in switch_mm() for user-to-user transitions without explicitly issuing one, creating a data race.

## Fix Summary

The fix introduces a new memory barrier primitive smp_mb__after_switch_mm() and explicitly calls it in the user-to-user transition path within switch_mm_cid(). This ensures the required memory ordering regardless of architecture. Architectures like x86 that already provide an implicit barrier in switch_mm() can override this with a no-op for efficiency.

## Triggering Conditions

The bug occurs during user-to-user task context switches when:
- Architecture's switch_mm() lacks implicit memory barrier (e.g., arm64)
- Two user tasks with different memory contexts switch between CPUs
- Race between storing to rq->curr and accessing mm_cid fields in switch_mm_cid()
- Concurrent sched_mm_cid_remote_clear() sets lazy_put but fails to observe active task
- Timing window exists between rq->curr update and switch_mm_cid() call
- Remote CPU incorrectly clears actively used mm_cid due to lack of visibility

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved for driver). Create two user tasks with different memory contexts:
- **Setup**: Use kstep_task_create() to create tasks on CPUs 1-2, configure different cgroups 
- **Trigger race**: Use kstep_tick_repeat() to induce frequent context switches between user tasks
- **Force user-to-user transitions**: Pin tasks with kstep_task_pin() then migrate between CPUs
- **Observe race window**: Use on_tick_begin() callback to log rq->curr and mm_cid state
- **Detection**: Check for incorrect mm_cid clearing by monitoring task->mm->pcpu_cid[cpu] values
- **Validation**: Compare buggy vs fixed kernel - buggy should show mm_cid corruption/clearing
- Use TRACE_INFO() to log mm_cid values and task transitions during critical windows
