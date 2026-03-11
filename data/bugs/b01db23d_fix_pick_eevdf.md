# sched/eevdf: Fix pick_eevdf()

- **Commit:** b01db23d5923a35023540edc4f0c5f019e11ac7d
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** EDF (Earliest Deadline First scheduling)

## Bug Description

The `pick_eevdf()` algorithm could fail to find the actual earliest eligible deadline when searching the rbtree. Specifically, when descending right to locate the minimum deadline in a subtree, the algorithm could find a `min_deadline` value that wasn't actually eligible for scheduling. This resulted in the scheduler selecting a suboptimal task instead of the task with the true earliest eligible deadline.

## Root Cause

The original algorithm used a single-pass descent through the rbtree, attempting to find the earliest deadline while checking eligibility only at nodes visited during that descent. When the minimum deadline was found in the right subtree but that deadline wasn't eligible, the algorithm failed to backtrack and search through left branches (which were skipped during the descent) for the actual best eligible deadline. The algorithm lacked logic to recognize when a found `min_deadline` was ineligible and required searching alternative branches.

## Fix Summary

The fix restructures the algorithm into two phases: the first phase descends while tracking both the best eligible deadline found so far and the best `min_deadline` in left branches skipped during descent. When the first phase finds that the best left branch's `min_deadline` is actually better than the best deadline found, a second search phase is triggered to search through that eligible left subtree for the actual earliest deadline. This ensures the scheduler always finds the true earliest eligible deadline, while maintaining O(log n) complexity.

## Triggering Conditions

The bug occurs in the EEVDF scheduler when multiple runnable tasks exist with different eligibility status and deadlines arranged in the rbtree such that:
- The algorithm descends right following `min_deadline` values in the augmented rbtree
- The task with the actual minimum deadline in the right subtree is not eligible for scheduling
- There exist eligible tasks in left branches that were skipped during the descent whose deadlines are earlier than any found eligible task
- The timing window where `pick_eevdf()` is called while this specific tree topology exists

## Reproduce Strategy (kSTEP)

Setup 3-4 CPUs with multiple CFS tasks having different priorities and create an rbtree topology where min_deadline points to ineligible tasks in right subtrees:
1. **Setup**: Create 4 tasks with different weights/priorities using `kstep_task_create()` and `kstep_task_set_prio()`
2. **Create imbalance**: Use `kstep_task_wakeup()` and `kstep_tick_repeat()` to build different vruntime/deadline values 
3. **Force eligibility gaps**: Use `kstep_task_pause()` on some tasks to create ineligible entities while keeping them in the tree
4. **Trigger scheduling**: Call `kstep_tick()` to invoke the scheduler during the problematic tree state
5. **Detection**: Use `on_tick_begin` callback to log selected task details and verify if the actual earliest eligible deadline was chosen by checking task deadlines and eligibility status
