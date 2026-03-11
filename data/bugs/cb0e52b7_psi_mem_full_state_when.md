# psi: Fix PSI_MEM_FULL state when tasks are in memstall and doing reclaim

- **Commit:** cb0e52b7748737b2cf6481fdd9b920ce7e1ebbdf
- **Affected file(s):** kernel/sched/psi.c, kernel/sched/stats.h
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

Memory FULL pressure (PSI_MEM_FULL) was significantly underreported for workloads where tasks enter memory reclaim while remaining on the runqueue. A single-threaded memory-bound task leaking memory would show significant memory SOME pressure but minimal FULL pressure, even though it was completely stalled on memory. The bug occurs because the kernel treats reclaiming tasks as simultaneously "in memstall" and "running productive work," when in reality these are the same task unable to make progress.

## Root Cause

The PSI_MEM_FULL condition incorrectly assumed that memory-stalled tasks must not be running: `tasks[NR_MEMSTALL] && !tasks[NR_RUNNING]`. This logic fails for reclaiming tasks, which are counted in both NR_MEMSTALL (stalled on memory) and NR_RUNNING (on the runqueue). The code was confused into thinking there was both a stalled task and a productive task when only a single reclaimer existed, causing FULL pressure metrics to be underestimated.

## Fix Summary

The fix introduces a new `TSK_MEMSTALL_RUNNING` counter to track tasks that are both in memory stall and actively running/reclaiming. It changes the PSI_MEM_FULL condition from `!tasks[NR_RUNNING]` to `tasks[NR_RUNNING] == tasks[NR_MEMSTALL_RUNNING]`, ensuring FULL state is reached when all running tasks are memory-stalled reclaimers. The fix also updates all code paths that enter/exit memstall to properly maintain this new state counter.

## Triggering Conditions

The bug requires tasks to enter memory reclaim while remaining on the runqueue, creating a scenario where `tasks[NR_MEMSTALL] > 0` and `tasks[NR_RUNNING] > 0` simultaneously. This occurs when:
- Memory pressure forces a task into page reclaim (calling `psi_memstall_enter()`)
- The task remains runnable/running during reclaim instead of sleeping
- The PSI accounting logic incorrectly treats the reclaiming task as both "stalled" and "productive"
- Common with memory-constrained cgroups where tasks hit memory.high limits
- CPU throttling (cpu.max) can exacerbate the issue by keeping reclaimers on runqueue longer
- The old condition `!tasks[NR_RUNNING]` fails because reclaimers are counted as running
- Results in PSI_MEM_FULL significantly underreporting actual memory stall time

## Reproduce Strategy (kSTEP)

Requires 2 CPUs minimum (CPU 0 reserved for driver). Create a cgroup with memory constraints and CPU throttling to force a task into prolonged memory reclaim:
- `setup()`: Call `kstep_cgroup_create("membound")` and `kstep_cgroup_set_weight("membound", 128)` for CPU throttling
- Create a task with `kstep_task_create()` and add to cgroup via `kstep_cgroup_add_task("membound", task->pid)`
- Pin task to CPU 1 with `kstep_task_pin(task, 1, 1)` to avoid migration
- `run()`: Wake task with `kstep_task_wakeup(task)` and simulate memory pressure through repeated task forks/exits
- Use `kstep_task_fork(task, 10)` followed by `kstep_tick_repeat(100)` to create memory allocation pressure
- Monitor PSI state via custom callbacks that read `/proc/pressure/memory` using `kstep_write()`
- Bug is triggered when PSI shows high memory SOME pressure but disproportionately low FULL pressure
- Check `tasks[NR_MEMSTALL]` and `tasks[NR_RUNNING]` counters to verify simultaneous non-zero values
- Log PSI_MEM_FULL calculations to confirm the old condition returns false when it should return true
