# sched/core: Avoid obvious double update_rq_clock warning

- **Commit:** 2679a83731d51a744657f718fc02c3b077e47562
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/rt.c, kernel/sched/sched.h
- **Subsystem:** RT scheduler, Deadline scheduler, core scheduler

## Bug Description

The kernel issues a WARN_DOUBLE_CLOCK warning when code paths use `raw_spin_rq_lock()` to directly acquire a runqueue lock and then call `update_rq_clock()`. This warning occurs when acquiring another CPU's runqueue lock while the current CPU's `rq->clock_update_flags` is already set to `RQCF_UPDATED`, causing the subsequent `update_rq_clock()` call to incorrectly trigger the double-update detection. The bug manifests in RT/DL scheduler operations (RT period timers, RT/DL task migration and balancing) with CONFIG_SCHED_DEBUG enabled.

## Root Cause

The root cause is a mismatch between lock acquisition methods: `raw_spin_rq_lock()` does not manage the `rq->clock_update_flags` (unlike `rq_lock()` which handles it), but `update_rq_clock()` checks these flags to detect double updates. When holding a raw spinlock on one CPU's runqueue while that CPU's flags are in `RQCF_UPDATED` state, the subsequent `update_rq_clock()` call incorrectly triggers the warning even though it's a legitimate single update operation.

## Fix Summary

The fix introduces `double_rq_clock_clear_update()` to clear the `RQCF_UPDATED` flag in `rq->clock_update_flags` before returning from double-lock operations. Additionally, `sched_rt_period_timer()` and `migrate_task_rq_dl()` are modified to use `rq_lock()/rq_unlock()` instead of `raw_spin_rq_lock()/raw_spin_rq_unlock()`, which properly manages the clock update flags. The function is called after all lock acquisitions in `double_rq_lock()` and `_double_lock_balance()` to ensure flags are cleared before any `update_rq_clock()` invocations.

## Triggering Conditions

This bug requires CONFIG_SCHED_DEBUG enabled and manifests in RT/DL scheduler cross-CPU operations. The warning triggers when: (1) Current CPU has `rq->clock_update_flags` set to `RQCF_UPDATED` from a previous clock update, (2) Code acquires another CPU's runqueue lock using `raw_spin_rq_lock()` instead of `rq_lock()`, (3) Subsequent `update_rq_clock()` call on the locked runqueue incorrectly detects a double update. Key scenarios include RT period timer handling (`sched_rt_period_timer()`), RT/DL task migration (`migrate_task_rq_dl()`), and RT task push/pull operations during load balancing (`push_rt_task()`, `pull_rt_task()`) that acquire locks across CPUs. The timing requires overlapping scheduler activity where one CPU's clock flags remain set while operations on other CPUs trigger the warning.

## Reproduce Strategy (kSTEP)

Use 3+ CPUs (CPU 0 reserved) to enable cross-CPU RT operations. Create RT tasks with `kstep_task_create()` + `kstep_task_fifo()`, pin them with `kstep_task_pin()` to different CPUs to create imbalanced loads. Set up RT throttling parameters via `kstep_sysctl_write("kernel.sched_rt_period_us", "100000")` and `kstep_sysctl_write("kernel.sched_rt_runtime_us", "50000")` to trigger RT period timer activity. In `run()`, create CPU imbalance by pinning multiple RT tasks to one CPU, then use `kstep_tick_repeat()` to advance time and trigger RT balancing. Monitor with `on_tick_begin()` callback logging RT runqueue states and task migrations. The bug manifests as WARN_DOUBLE_CLOCK kernel warnings in dmesg during RT task push/pull operations or period timer handling. Use `kstep_task_wakeup()` and `kstep_task_pause()` to create migration scenarios that trigger cross-CPU lock acquisition patterns.
