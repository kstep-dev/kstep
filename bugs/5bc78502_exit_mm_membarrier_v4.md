# sched: fix exit_mm vs membarrier (v4)

- **Commit:** 5bc78502322a5e4eef3f1b2a2813751dc6434143
- **Affected file(s):** kernel/sched/membarrier.c
- **Subsystem:** core

## Bug Description

When a thread exits and clears `current->mm`, a concurrent membarrier call may skip sending IPIs to CPUs because their `current->mm` is NULL, yet user-space memory accesses performed before `exit_mm` are not properly ordered. This violates membarrier's memory ordering guarantees and can occur with thread groups created using `CLONE_VM` without `CLONE_THREAD`. The race allows a thread in a different thread group to observe unordered memory accesses despite issuing a membarrier, leading to consistency violations.

## Root Cause

The runqueue's `membarrier_state` was not synchronized with the current mm's state during task exit. When `exit_mm` clears `current->mm` to NULL, the membarrier system call checks the runqueue's `membarrier_state` to determine whether to send IPIs. If this state is stale (indicating membarrier is needed) but the mm is already NULL, membarrier skips the IPI, assuming the task no longer participates in membarrier synchronization. However, any memory accesses by the exiting task prior to clearing mm are not properly ordered relative to the membarrier call.

## Fix Summary

The fix introduces `membarrier_update_current_mm()` to synchronize the runqueue's `membarrier_state` with the current mm's state. By keeping these states in sync, when `exit_mm` clears the mm, the runqueue state is updated accordingly, ensuring membarrier either sends the necessary IPI before the mm is cleared or correctly determines no IPI is needed because the state is already synchronized.

## Triggering Conditions

The bug requires a race between task exit and membarrier system call where:
- Two thread groups (A and B) share virtual memory (`CLONE_VM`) but not thread state (`!CLONE_THREAD`)
- Thread A (CPU 0) performs memory writes, then calls `exit()` → `exit_mm()` → clears `current->mm` to NULL
- Thread B (CPU 1) concurrently calls `membarrier()` after Thread A's memory writes but while/after `current->mm` is NULL
- The runqueue's `membarrier_state` is stale (not synchronized with mm state) when `exit_mm` occurs
- Timing: Thread A's memory accesses must be visible to Thread B before the membarrier call, but membarrier skips IPI to CPU 0 because `current->mm == NULL`, violating ordering guarantees

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create two tasks sharing memory but in different thread groups:
- Setup: Create two tasks with `kstep_task_create()`, pin to different CPUs with `kstep_task_pin(task_a, 1, 1)` and `kstep_task_pin(task_b, 2, 2)`
- Enable membarrier on the shared mm by simulating membarrier registration calls
- In run(): Task A performs memory operations then exits via `kstep_task_pause()` (simulating exit_mm clearing current->mm)
- Use timing control with `kstep_tick()` to precisely coordinate: Task A writes → exits, then Task B calls membarrier
- Monitor via custom callbacks: check runqueue `membarrier_state` vs mm state during `on_tick_begin()` 
- Detect bug: observe that membarrier skips IPI when `current->mm == NULL` but runqueue state indicates IPI needed
- Use `kstep_cgroup_create()` and `kstep_cgroup_add_task()` to isolate thread groups if needed for CLONE_VM/!CLONE_THREAD semantics
