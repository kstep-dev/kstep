# sched/mmcid: Revert the complex CID management

- **Commit:** 77d7dc8bef482e987036bc204136bbda552d95cd
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core

## Bug Description

The complex CID (Concurrency ID) management mechanism causes latency spikes when tasks exit to user space. The compaction mechanism forces random tasks of a process into task work during exit, creating unpredictable and undesirable latency behavior on task transitions.

## Root Cause

The complex CID compaction and remote-clear mechanism attempts to optimize memory utilization by compacting allocated CIDs, but in doing so it schedules task work on arbitrary tasks during context switching and task exit. This introduces unpredictable latency when transitioning to user space, as tasks may be forced to execute the CID cleanup work at inopportune times.

## Fix Summary

The fix reverts the complex CID management back to the simpler initial bitmap allocation mechanism. This eliminates the latency spikes caused by forced task work during exit, though it re-introduces known scalability limitations. This revert allows for a cleaner, more reviewable approach to building better CID management in subsequent changes.

## Triggering Conditions

The bug occurs when the complex CID compaction mechanism forces task work onto arbitrary tasks during exit to user space. The specific conditions include:
- Tasks with associated mm_struct that hold per-CPU CIDs in a multi-threaded process
- CID compaction algorithms scanning and clearing remote CIDs on other CPUs 
- Context switches involving tasks with different memory contexts (task->mm transitions)
- Task exit paths where CID cleanup work gets deferred via task_work to random tasks
- Race conditions between `rq->curr` updates during context switch and remote CID clearing
- Memory barriers in `switch_mm_cid()` coordination that triggers forced compaction work
- The timing when `task_tick_mm_cid()` runs during scheduler tick processing

## Reproduce Strategy (kSTEP)

Create multiple CPUs (need at least 3: driver on CPU 0, test workload on CPUs 1-2) and simulate multi-threaded workload with frequent task exits:
- In `setup()`: Use `kstep_task_create()` to create multiple tasks sharing mm_struct via `kstep_task_fork()`
- Configure tasks to run on different CPUs with `kstep_task_pin()` to trigger cross-CPU CID management
- In `run()`: Repeatedly cycle tasks through create/wakeup/pause/exit using `kstep_task_wakeup()` and `kstep_task_pause()`
- Use `kstep_tick_repeat()` to advance scheduler ticks and trigger `task_tick_mm_cid()` processing
- Monitor `on_tick_end()` callback to detect latency spikes during task exit to user space transitions
- Check for forced task work execution by logging context switch paths in `on_tick_begin()`
- Detect bug by measuring timing variations in task exit latency via `kstep_json_field_u64()` logging
