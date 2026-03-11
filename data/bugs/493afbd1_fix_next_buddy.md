# sched/fair: Fix NEXT_BUDDY

- **Commit:** 493afbd187c4c9cc1642792c0d9ba400c3d6d90d
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

When NEXT_BUDDY is enabled on systems with delayed dequeue support, entities marked as next buddy can incorrectly remain as delayed entities. This violates a scheduler invariant checked in pick_next_entity(), causing a WARN to be triggered whenever NEXT_BUDDY selection interacts with the delayed dequeue mechanism.

## Root Cause

The bug arises from two issues: (1) clear_buddies() was called too late in dequeue_entity(), after delayed dequeue logic had already run, allowing a buddy designation to survive into delayed state; (2) set_next_buddy() in check_preempt_wakeup_fair() did not check whether the candidate entity was already delayed, permitting delayed entities to be assigned as next buddies.

## Fix Summary

Move clear_buddies() earlier in dequeue_entity() to execute before delayed dequeue logic. Additionally, add a check in check_preempt_wakeup_fair() to prevent setting a delayed entity as next buddy, ensuring the invariant that next buddy entities are never delayed is maintained.

## Triggering Conditions

The bug triggers when NEXT_BUDDY and DELAY_DEQUEUE features are both enabled. An entity becomes next buddy, then enters the delayed dequeue path when sleeping while ineligible (having positive vlag). The delayed dequeue logic preserves entities in delayed state instead of fully dequeuing them, but buddy clearing happened too late, allowing buddy status to persist. When pick_next_entity() later encounters a delayed entity marked as next buddy, it triggers a WARN due to the violated invariant that buddies should never be delayed. The race requires precise timing where buddy assignment happens followed by sleep-dequeue while the entity is ineligible.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create 3 CFS tasks: task_a (nice 1), task_b (nice 0), task_c (nice 0). In setup(), enable NEXT_BUDDY and DELAY_DEQUEUE features via sysctl. In run(), wake tasks A and B, run for several ticks to build different vruntime/vlag values due to weight differences. Pause task B to trigger vlag calculation, then run A alone to advance min_vruntime. Wake task B (will have positive vlag, be ineligible, and become delayed when sleeping). Create task C that preempts B in check_preempt_wakeup_fair(), causing B to be set as next buddy despite being delayed. Use on_tick_end() callback to check cfs_rq->next and verify if it points to a delayed entity (se->sched_delayed). Trigger pick_next_entity() and monitor for WARN via kernel logs to detect the invariant violation.

