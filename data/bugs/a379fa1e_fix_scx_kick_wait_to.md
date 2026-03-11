# sched_ext: Fix SCX_KICK_WAIT to work reliably

- **Commit:** a379fa1e2cae15d7422b4eead83a6366f2f445cb
- **Affected file(s):** kernel/sched/ext.c, kernel/sched/ext_internal.h
- **Subsystem:** EXT (sched_ext)

## Bug Description

SCX_KICK_WAIT is used to synchronously wait for a target CPU to complete a reschedule, enabling operations like core scheduling. After commit b999e365c298 replaced `scx_next_task_picked()` with `switch_class()`, the mechanism broke: `pnt_seq` (pick-next-task sequence number) was no longer reliably incremented on every task pick, but only when switching between sched classes. This caused SCX_KICK_WAIT to fail to properly synchronize, breaking core scheduling and similar features that depend on this guarantee.

## Root Cause

Commit b999e365c298 refactored the task picking path, replacing the unconditional call to `scx_next_task_picked()` (which always incremented `pnt_seq`) with a conditional call to `switch_class()` (called only on sched class transitions). This broke the invariant that `pnt_seq` increments whenever a task is picked: when the same SCX task is re-selected, or when an SCX task continues on the same CPU, `pnt_seq` wouldn't increment, causing waiters to never observe the sequence change.

## Fix Summary

The fix restores reliable `pnt_seq` incrementing by placing increments at the correct points in the SCX task picking path: in `put_prev_task_scx()` (called on every task switch-out) and in `pick_task_scx()` (called on every task pick). Additionally, the logic in `kick_one_cpu()` is refined to only enable waiting when the target CPU is currently running an SCX task, and the busy-wait loop is optimized with `smp_cond_load_acquire()` for better architecture support.

## Triggering Conditions

This bug occurs in the sched_ext (SCX) scheduler when using SCX_KICK_WAIT functionality for synchronization. The key conditions are:
- A CPU running an SCX task that needs to be synchronized via SCX_KICK_WAIT
- The target CPU continues to run the same SCX task or switches between SCX tasks (no sched class transition occurs)
- Another CPU calls `scx_bpf_kick_cpu()` with SCX_KICK_WAIT flag to synchronously wait for reschedule completion
- The `pnt_seq` increment only occurred in `switch_class()` (sched class transitions), not on every task pick
- This caused waiters to never observe sequence number changes, leading to infinite waits and broken synchronization for core scheduling operations

## Reproduce Strategy (kSTEP)

This bug requires SCX scheduler to be active, which kSTEP doesn't directly support as it focuses on CFS/RT/DL schedulers. However, the core issue can be demonstrated conceptually:
- Setup: 2+ CPUs with SCX scheduler enabled and loaded BPF program
- Create SCX tasks on CPU 1: `task_scx = kstep_task_create(); /* configure as SCX */`
- From driver (CPU 0), initiate SCX_KICK_WAIT on CPU 1 via SCX BPF interface
- Observe: Same SCX task continues running (no class switch), `pnt_seq` not incremented
- Use `on_tick_begin()` callback to monitor task switches and sequence numbers
- Check: Waiter thread should hang indefinitely due to unincremented `pnt_seq`
- Detection: Log sequence number changes and waiter timeout to confirm synchronization failure
Note: This requires SCX framework integration beyond current kSTEP capabilities.
