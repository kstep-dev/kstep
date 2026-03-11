# sched/cputime: Fix getrusage(RUSAGE_THREAD) with nohz_full

- **Commit:** e7f2be115f0746b969c0df14c0d182f65f005ca5
- **Affected file(s):** kernel/sched/cputime.c
- **Subsystem:** cputime/core

## Bug Description

getrusage(RUSAGE_THREAD) with nohz_full returns shorter utime/stime values than the actual time spent by the thread. The bug occurs when a thread is running with the tick disabled (nohz_full mode), causing the reported CPU time to be inconsistent with the actual execution time. This can lead to incorrect profiling and resource accounting in applications relying on getrusage().

## Root Cause

task_cputime_adjusted() snapshots the thread's utime and stime, then adjusts their sum to match the scheduler-maintained sum_exec_runtime. However, in nohz_full mode, sum_exec_runtime is only updated once per second in the worst case, while utime and stime can be updated anytime via vtime accounting. This creates a discrepancy where the adjusted values become inconsistent because they rely on a stale sum_exec_runtime value.

## Fix Summary

The fix modifies task_cputime() to return a boolean indicating whether the task is actually running (vtime->state >= VTIME_SYS). When task_cputime_adjusted() detects the task is running, it refreshes sum_exec_runtime by calling task_sched_runtime(p) before performing the cputime adjustment. This ensures the accounting values are synchronized with current runtime state.

## Triggering Conditions

The bug requires CONFIG_VIRT_CPU_ACCOUNTING_GEN enabled with nohz_full CPUs. A task must be running continuously on a nohz_full CPU with the tick disabled for an extended period (worst case: ~1 second). During this time, the task accumulates utime/stime through vtime accounting while sum_exec_runtime becomes stale. The discrepancy manifests when getrusage(RUSAGE_THREAD) is called via task_cputime_adjusted(), which attempts to reconcile the fresh utime/stime values against the outdated sum_exec_runtime, leading to underreported CPU time.

## Reproduce Strategy (kSTEP)

Configure a nohz_full CPU using kstep_sysctl_write("kernel.nohz_full", "1"). Create a CPU-bound task that will run uninterrupted on CPU 1 using kstep_task_create() and kstep_task_pin(task, 1, 1). In setup(), enable vtime accounting if available. In run(), wake the task with kstep_task_wakeup() and let it run for many ticks using kstep_tick_repeat(1000) to accumulate significant runtime while keeping the tick disabled. Use kstep_task_usleep() to simulate periodic getrusage() calls that trigger task_cputime_adjusted(). Monitor the task's utime, stime, and sum_exec_runtime values through custom kernel logging to detect the staleness discrepancy. The bug is triggered when sum_exec_runtime lags significantly behind the sum of utime and stime.
