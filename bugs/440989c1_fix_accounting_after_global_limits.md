# sched/deadline: Fix accounting after global limits change

- **Commit:** 440989c10f4e32620e9e2717ca52c3ed7ae11048
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/rt.c
- **Subsystem:** Deadline, RT

## Bug Description

When global RT limits are changed via sysctl (sched_rt_period or sched_rt_runtime), the deadline scheduler's per-runqueue bandwidth accounting variables (such as extra_bw and max_bw) become stale or incorrect. This leaves scheduling decisions operating on invalid accounting state, potentially causing incorrect task placement and priority handling for deadline tasks.

## Root Cause

The deadline scheduler's per-runqueue bandwidth accounting is initialized based on global limits, but when those limits are changed in sched_rt_handler(), the old accounting values are not reset before the new limits are applied. Additionally, scheduling domains (which depend on correct accounting) are not rebuilt after the change, causing them to operate with inconsistent state.

## Fix Summary

The fix resets per-runqueue deadline bandwidth accounting (via init_dl_rq_bw_ratio) for all CPUs before processing the new global bandwidth value, ensuring no stale values remain. After all updates are complete, rebuild_sched_domains() is called to recompute scheduling domains and per-domain variables based on the correct accounting state.

## Triggering Conditions

The bug is triggered in the deadline scheduler's sched_dl_do_global() function when global RT bandwidth limits are modified via sysctl (sched_rt_period_us or sched_rt_runtime_us). The problematic sequence occurs when:
- Global RT limits are changed through sysctl interface, calling sched_dl_do_global()
- Per-CPU deadline bandwidth structures (dl_b) are updated with new bandwidth values
- init_dl_rq_bw_ratio() is called per-CPU after dl_b->bw updates, creating inconsistent state
- The dl_rq->bw_ratio, max_bw, and extra_bw variables become stale relative to the new dl_b->bw
- Subsequent deadline task admission and bandwidth enforcement decisions operate on inconsistent accounting
- Race conditions may occur if deadline tasks are being scheduled during the sysctl update

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). In setup(), create deadline tasks on CPU 1-2 and establish initial RT bandwidth limits. In run():
- Create deadline tasks with kstep_task_create() and set SCHED_DEADLINE policy
- Read initial per-CPU dl_rq bandwidth accounting values (bw_ratio, max_bw, extra_bw)  
- Modify global RT limits using kstep_sysctl_write("kernel.sched_rt_period_us", "1000000") and kstep_sysctl_write("kernel.sched_rt_runtime_us", "950000")
- Immediately after sysctl write, examine per-CPU dl_rq accounting on multiple CPUs
- Compare dl_rq->bw_ratio/max_bw/extra_bw values against corresponding dl_b->bw values for consistency
- Use on_tick_begin() callback to log bandwidth accounting state during deadline task execution
- Look for mismatched bw_ratio calculations or stale extra_bw values that don't reflect new global limits
- Detection: inconsistent dl_rq accounting relative to global bandwidth or admission control failures
