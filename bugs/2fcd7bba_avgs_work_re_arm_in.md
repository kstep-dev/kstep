# sched/psi: Fix avgs_work re-arm in psi_avgs_work()

- **Commit:** 2fcd7bbae90a6d844da8660a9d27079281dfbba2
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

PSI avgs_work idle shutoff is not working at all. When only the kworker running avgs_work is on the CPU, psi_avgs_work() always re-arms itself instead of shutting down during idle periods. This prevents the system from entering an idle state for PSI monitoring, causing unnecessary CPU overhead and preventing proper idle shutdown of the avgs_work.

## Root Cause

The psi_avgs_work() function checks the PSI_NONIDLE delta to determine whether to re-arm the work. However, on the current CPU running avgs_work, PSI_NONIDLE will always be true because the kworker itself is counted as a non-idle task, regardless of other system activity. This causes the re-arm condition to always trigger, preventing the idle shutoff mechanism from ever working.

## Fix Summary

The fix introduces a PSI_STATE_RESCHEDULE flag to more intelligently decide when to re-arm avgs_work. For the current CPU, it only re-arms when there are other tasks beyond the kworker itself (NR_RUNNING > 1 || NR_IOWAIT > 0 || NR_MEMSTALL > 0). For other CPUs, it checks the PSI_NONIDLE delta as before, allowing proper idle shutoff when the system is truly idle.

## Triggering Conditions

The bug occurs in the PSI subsystem's pressure monitoring framework when:
- PSI avgs_work kworker is the only task running on a CPU
- The system enters what should be an idle state for PSI monitoring
- psi_avgs_work() calls get_recent_times() which detects PSI_NONIDLE due to the kworker itself
- The re-arm logic always triggers because PSI_NONIDLE delta is always true on the current CPU
- This prevents the idle shutoff mechanism from working, causing continuous re-arming

## Reproduce Strategy (kSTEP)

Create a minimal system state where PSI avgs_work should shut off but doesn't:
- Use at least 2 CPUs (CPU 0 reserved for driver, CPU 1 for test)  
- Setup: Enable PSI if not already enabled via /proc/pressure/* 
- Run: Create a single task on CPU 1, let it run briefly to trigger PSI activity
- Then pause/kill the task to create an "idle" state where only PSI kworker should remain
- Monitor: Use on_tick_begin() callback to check if PSI avgs_work keeps re-arming
- Detection: Log when psi_avgs_work() executes vs expected shutoff timing
- Expected: In buggy kernel, avgs_work continues indefinitely; in fixed kernel, it shuts off
