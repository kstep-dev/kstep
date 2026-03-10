# sched/debug: Fix requested task uclamp values shown in procfs

- **Commit:** ad32bb41fca67936c0c1d6d0bdd6d3e2e9c5432f
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** debug

## Bug Description

The uclamp debug output in procfs was displaying the effective uclamp values instead of the requested values when printing task uclamp data in /proc/<pid>/sched_debug. Users could not see what uclamp values were actually requested, only the effective values that were already printed separately on the next line, making the debug output duplicative and incorrect.

## Root Cause

The code was reading from `p->uclamp` which holds the last effective values, not the requested values. The distinction between `p->uclamp_req` (requested) and `p->uclamp` (effective) is critical for understanding uclamp behavior, but the debug code was printing the same data twice instead of showing both requested and effective values.

## Fix Summary

Changed two lines in the procfs debug output to read from `p->uclamp_req` instead of `p->uclamp` when printing requested uclamp values, ensuring that users see both the requested values and effective values distinctly.

## Triggering Conditions

This bug manifests whenever uclamp is enabled (`CONFIG_UCLAMP_TASK=y`) and task uclamp values differ from their effective values. The trigger conditions require:
- A task with explicit uclamp.min or uclamp.max values set via sched_setattr() or cgroup configuration
- System-wide uclamp limits or cgroup hierarchy constraints that modify the effective values
- Reading `/proc/<pid>/sched` to display the debug output
- The bug occurs when `p->uclamp_req` (requested) differs from `p->uclamp` (effective)
- No specific CPU topology, task states, or race conditions required - purely a debug output issue
- Bug is visible immediately upon reading procfs when uclamp constraints are active

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create a task with explicit uclamp values that differ from effective values:
- In `setup()`: Create cgroup with restrictive uclamp limits using `kstep_cgroup_create()` and `kstep_cgroup_write()`
- Create task with `kstep_task_create()` and set higher uclamp values via task-level uclamp interface
- Use `kstep_cgroup_add_task()` to add task to restrictive cgroup, creating req vs effective mismatch
- In `run()`: Let task run briefly with `kstep_tick_repeat(5)` to establish uclamp state
- Use procfs reading mechanism to capture debug output showing both requested and effective values
- Compare the "uclamp.min" and "effective uclamp.min" lines in output
- Bug detected when both lines show identical values instead of requested vs effective distinction
- Success when requested values differ from effective values in debug output
