# sched/deadline: Fix bandwidth reclaim equation in GRUB

- **Commit:** 6a9d623aad89539eca71eb264db6b9d538620ad5
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/sched.h
- **Subsystem:** Deadline

## Bug Description

The GRUB (Greedy Reclamation of Unused Bandwidth) algorithm for deadline scheduling was not reclaiming the maximum allowed bandwidth for deadline tasks. According to the GRUB rule, runtime should be decreased as `dq = -(max{u, (Umax - Uinact - Uextra)} / Umax) dt`, but the implementation had an incorrect equation that failed to apply the division by Umax correctly. This caused deadline tasks to receive significantly less than their entitled bandwidth, underutilizing the CPU and reducing scheduler efficiency.

## Root Cause

The bug was in the `grub_reclaim()` function's calculation of reclaimed bandwidth. The old code computed `u_act_min` by multiplying the task's bandwidth by `bw_ratio` and shifting, then used this value in the max operation but never completed the division-by-Umax step required by the algorithm. Additionally, a new field `max_bw` was not being tracked, which should store the maximum available bandwidth for reclamation. This incorrect ordering and missing division caused the reclamation to produce values significantly smaller than the theoretical maximum.

## Fix Summary

The fix introduces a new `max_bw` field to track maximum available bandwidth and corrects the reclamation equation. The code now properly computes `u_act` as the maximum of the task's bandwidth or the remaining available bandwidth, then applies the `bw_ratio` scaling and division-by-Umax step as required by the GRUB algorithm. Test results demonstrate that with this fix, deadline tasks correctly reclaim bandwidth up to the Umax limit (95%) instead of underutilizing the CPU.

## Triggering Conditions

The bug manifests when deadline tasks with SCHED_FLAG_RECLAIM attempt to reclaim unused bandwidth through the GRUB algorithm. Triggering requires:
- At least one SCHED_DEADLINE task with runtime < deadline/period (creating unused bandwidth) 
- The task must be in active execution triggering `grub_reclaim()` in `update_curr_dl()`
- Either low task bandwidth relative to Umax, or multiple competing deadline tasks
- The bug is more pronounced with smaller task bandwidths where the incorrect `u_act_min` computation significantly underestimates available bandwidth
- System must have deadline bandwidth limit < 100% (typical default is 95% via `/proc/sys/kernel/sched_rt_{runtime,period}_us`)

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. Create deadline tasks with varying bandwidths to expose the reclamation miscalculation:
- In `setup()`: Create 2-3 deadline tasks with different runtime/deadline ratios (e.g., 1ms/10ms, 1ms/100ms)
- Use `kstep_cgroup_create()` and set tasks with `SCHED_FLAG_RECLAIM` 
- In `run()`: Use `kstep_task_pin()` to pin tasks to CPU 1, then `kstep_task_wakeup()` 
- Execute tasks with `kstep_tick_repeat()` for sustained periods (100+ ticks)
- Use `on_tick_begin()` callback to monitor actual CPU utilization vs. theoretical maximum
- Log bandwidth reclamation via custom printk in `grub_reclaim()` to observe `u_act` values
- Bug detected when aggregate utilization stays significantly below 95% despite available bandwidth
- Compare utilization before and after the fix to confirm reclamation reaches Umax limit
