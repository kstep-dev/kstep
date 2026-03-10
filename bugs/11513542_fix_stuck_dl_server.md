# sched/deadline: Fix 'stuck' dl_server

- **Commit:** 115135422562e2f791e98a6f55ec57b2da3b3a95
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

The dl_server (deferrable deadline server) could get stuck when dl_server_start() was called after a deadline had lapsed. Specifically, when transitioning from state A (no more runnable tasks) to state B (replenish required), the dl_defer_running flag remained set to 1, but the deadline scheduling logic expected it to be 0. This mismatch prevented the zero-laxity timer from starting properly, causing the server to fail to schedule work.

## Root Cause

The bug occurred in the state transition [4] D->A (dl_server_stop) followed by [1] A->B (dl_server_start with lapsed deadline). When update_dl_entity() detected an expired deadline and called replenish_dl_new_period(), it did not clear the dl_defer_running flag. State B's initialization logic, which depends on dl_defer_running==0, was therefore unable to properly arm the deferred timer, leaving the server stuck.

## Fix Summary

The fix adds an explicit `dl_se->dl_defer_running = 0;` statement before calling replenish_dl_new_period() in update_dl_entity(). This ensures that when a deadline has lapsed and the period needs to be replenished, the deferred running state is cleared, allowing the server to properly transition to state B with the correct timer configuration.

## Triggering Conditions

The bug occurs when a deferrable deadline server undergoes a specific state transition sequence:
- State [4] D->A: dl_server_stop() called due to no more runnable tasks, leaving dl_defer_running=1
- State [1] A->B: dl_server_start() called after the deadline has already lapsed
- The expired deadline triggers update_dl_entity() → replenish_dl_new_period()
- dl_defer_running remains 1, but state B requires dl_defer_running=0 for proper timer setup
- The zero-laxity timer fails to arm correctly, causing the server to become unresponsive
- Requires a deferrable dl_server with dl_defer flag set and precise timing of task availability

## Reproduce Strategy (kSTEP)

Setup: Use 2 CPUs (driver on CPU 0, server on CPU 1). Create a deferrable deadline server using cgroups.
1. Call kstep_cgroup_create("dl_server") and configure with bandwidth and defer settings
2. Create a task via kstep_task_create() and assign to the dl_server cgroup  
3. Wake the task, let it run briefly, then pause it to trigger D->A transition (dl_server_stop)
4. Use kstep_tick_repeat() to advance time past the server's deadline
5. Wake the task again to trigger A->B transition with expired deadline
6. In on_tick_begin() callback, check dl_se->dl_defer_running and dl_se->dl_throttled states
7. Monitor server behavior through multiple ticks - stuck server will show dl_defer_running=1
8. Log dl_se fields via printk in callbacks to detect when zero-laxity timer fails to start
9. Success: server remains responsive; Failure: server stuck with dl_defer_running=1 and no timer
