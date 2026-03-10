# psi: Fix race when task wakes up before psi_sched_switch() adjusts flags

- **Commit:** 7d9da040575b343085287686fa902a5b2d43c7ca
- **Affected file(s):** kernel/sched/core.c, kernel/sched/stats.h
- **Subsystem:** PSI (Pressure Stall Information), Core Scheduler

## Bug Description

A race condition occurs when a task wakes up during newidle balance (when the runqueue lock is temporarily released in `__schedule()`) before `psi_sched_switch()` has adjusted the PSI flags for the blocked task. This causes inconsistent PSI state tracking, manifesting as a kernel warning: "psi: inconsistent task state!" The bug was triggered in production with hackbench running in a cgroup with bandwidth throttling enabled.

## Root Cause

The DELAY_DEQUEUE feature introduced a scenario where blocked tasks remain on the runqueue with delayed dequeue semantics. When `psi_dequeue()` is called, it relies on `psi_sched_switch()` to set correct PSI flags. However, if a task wakes up and `psi_enqueue()` is called before `psi_sched_switch()` executes, the PSI state transition logic incorrectly treats the task as running when it should still be treated as blocked, leading to invalid flag state changes.

## Fix Summary

The fix addresses this by: (1) replacing the stale `block` variable with a runtime check using `task_on_rq_queued()` and `p->se.sched_delayed` to determine actual task state at the point of `psi_sched_switch()`, and (2) adding an early bailout in `psi_enqueue()` that skips PSI adjustments if the task is currently executing on a CPU, deferring all state transitions to `psi_sched_switch()`.

## Triggering Conditions

- Tasks must be subject to bandwidth throttling (cgroup with limited CPU bandwidth)
- DELAY_DEQUEUE feature must be enabled (kernel 6.6+ with CONFIG_SCHED_CFS_BANDWIDTH)
- Task becomes blocked and triggers delayed dequeue semantics on its runqueue
- The task's CPU must enter newidle balance, releasing the runqueue lock temporarily in `__schedule()`
- Another CPU in the same LLC domain must attempt to wake up the blocked task
- Race window: task wakes up on throttled hierarchy after `psi_dequeue()` but before `psi_sched_switch()` executes
- PSI state inconsistency manifests as "psi: inconsistent task state!" kernel warning

## Reproduce Strategy (kSTEP)

Create bandwidth throttling scenario with 3+ CPUs (0 reserved for driver, 1+ for workload):
1. **Setup**: Use `kstep_cgroup_create()` to create throttled cgroup, `kstep_cgroup_write("test", "cpu.max", "50000 100000")` for 50% bandwidth
2. **Task creation**: Multiple tasks via `kstep_task_create()`, add to cgroup with `kstep_cgroup_add_task()` 
3. **Load generation**: Pin tasks across CPUs using `kstep_task_pin()`, create CPU contention with `kstep_tick_repeat()`
4. **Race trigger**: Force task blocking/waking pattern with `kstep_task_pause()` and `kstep_task_wakeup()` during busy periods
5. **Detection**: Monitor PSI state via `on_tick_begin()` callback, log task states and cgroup throttling status
6. **Verification**: Check kernel logs for "inconsistent task state" warnings, verify PSI flag mismatches
