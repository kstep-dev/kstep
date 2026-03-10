# sched: Fix UCLAMP_FLAG_IDLE setting

- **Commit:** ca4984a7dd863f3e1c0df775ae3e744bff24c303
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The UCLAMP_FLAG_IDLE flag becomes stuck in the set state when update_uclamp_active() temporarily reaches zero active uclamp tasks during its dec-then-inc operation. This leaves the runqueue incorrectly marked as idle even though active tasks are present, breaking the scheduling state that depends on this flag for maintaining proper uclamp.max clamping.

## Root Cause

An asymmetry exists in flag management: uclamp_rq_dec_id() sets UCLAMP_FLAG_IDLE when reaching zero active tasks, while uclamp_rq_inc_id() clears it on enqueue. However, when update_uclamp_active() calls both dec and inc operations sequentially on the same task, the transient zero-task state triggers the flag set during dec, but the immediately following inc fails to clear it because the flag-clearing logic in uclamp_rq_inc() has different conditions than the flag-setting logic in dec.

## Fix Summary

Introduces uclamp_rq_reinc_id() helper that wraps the dec-then-inc operation and explicitly clears UCLAMP_FLAG_IDLE after re-incrementing. This ensures the flag is properly cleared whenever both operations complete together, fixing the asymmetry in flag management.

## Triggering Conditions

The bug requires uclamp to be enabled and a task with active uclamp constraints undergoing the update_uclamp_active() code path. This occurs when task properties change (nice values, cgroup membership, etc.) which triggers uclamp_rq_dec_id() followed by uclamp_rq_inc_id() on the same task. The critical condition is that the runqueue must transiently reach zero active uclamp tasks during the dec operation, causing UCLAMP_FLAG_IDLE to be set, while the subsequent inc operation fails to clear it due to the asymmetric flag management logic. This leaves the runqueue incorrectly marked as idle despite having active tasks.

## Reproduce Strategy (kSTEP)

Requires 1 CPU (CPU 1, with CPU 0 reserved for driver). In setup(), enable uclamp via sysctl and create a task with uclamp.max constraint using cgroups. In run(), use kstep_task_wakeup() to enqueue the task, then trigger update_uclamp_active() by changing the task's nice value with kstep_task_set_prio(). Use on_tick_end() callback to monitor the runqueue's uclamp_flags field, logging when UCLAMP_FLAG_IDLE is incorrectly set while nr_running > 0. The bug manifests as the flag remaining set after the priority change, even though the task is still active. Detection involves checking if (rq->uclamp_flags & UCLAMP_FLAG_IDLE) && rq->nr_running > 0 after the property change completes.
