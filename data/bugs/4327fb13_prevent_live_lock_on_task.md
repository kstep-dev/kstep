# sched/mmcid: Prevent live lock on task to CPU mode transition

- **Commit:** 4327fb13fa47770183c4850c35382c30ba5f939d
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core (MM_CID management)

## Bug Description

A live lock occurs when the scheduler switches from per-task MM_CID ownership to per-CPU ownership mode, particularly when a task (T1) schedules in on one CPU and is then migrated to another CPU before the fixup phase completes. When T1 migrates to the new CPU, that CPU's CID pool is empty (because T1's original CID was transferred to the first CPU), causing T1 to loop infinitely in mm_get_cid() while holding the runqueue lock. Meanwhile, the fixup task (T0) reaches T1 in the thread walk and blocks trying to acquire the same runqueue lock, resulting in a full system live lock.

## Root Cause

During the task-to-CPU mode transition, the fixup function directly transferred task-owned CIDs to CPU ownership or dropped them. However, when a task schedules in before the fixup reaches it, it may find no CIDs available on its current CPU and loop indefinitely trying to allocate one, blocking the fixup thread which is the only mechanism that can free CIDs. The reverse transition (CPU-to-task) avoided this issue by using a transit mode flag, but task-to-CPU did not.

## Fix Summary

The fix applies the same two-phase transit mechanism used in CPU-to-task transitions to the task-to-CPU mode switch. CIDs are marked with MM_CID_TRANSIT during the transition, signaling that they are temporarily owned by tasks. When a task schedules out with a transit CID, it drops the CID back into the pool. The fixup thread then clears the transit flag after completing the ownership transfer, preventing CID space exhaustion and live lock scenarios.

## Triggering Conditions

The live lock requires a precise race during MM_CID task-to-CPU mode transition:
- More threads than available CPUs (>4 in typical setup) to trigger per-CPU ownership mode
- Task migration during the fixup phase: a task (T1) schedules in on CPU1, transfers its CID to CPU1, then migrates to CPU2 before fixup reaches it
- CID pool exhaustion: CPU2 has no available CIDs since T1's original CID was transferred to CPU1
- T1 loops infinitely in mm_get_cid() while holding CPU2's runqueue lock
- Fixup thread (T0) reaches T1 in thread walk and blocks on the same runqueue lock
- System-wide live lock: no progress possible as both critical threads are blocked

## Reproduce Strategy (kSTEP)

Requires at least 5 CPUs (CPU 0 reserved for driver). Create 5+ tasks sharing the same memory space to trigger per-CPU mode transition:
- In setup(): Use kstep_task_create() to create 5 tasks (T0-T4), ensure they share mm_struct for MM_CID management
- Pin initial tasks: T0 on CPU1, T1 on CPU2, T2 on CPU3, T3 on CPU4 using kstep_task_pin()
- In run(): Wake T4 (5th task) with kstep_task_wakeup() to trigger task-to-CPU mode switch
- Immediately migrate T1 from CPU2 to CPU3 using kstep_task_pin(T1, 3, 3) during transition
- Use on_tick_begin() callback to monitor runqueue states and detect infinite loops in mm_get_cid()
- Detection: Log when tasks hold runqueue locks for extended periods and when CID allocation fails
- Success criteria: Observe T1 spinning with runqueue lock held while T0 (fixup) blocks on same lock
