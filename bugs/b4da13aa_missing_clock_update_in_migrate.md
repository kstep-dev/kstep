# sched/deadline: Fix missing clock update in migrate_task_rq_dl()

- **Commit:** b4da13aa28d4fd0071247b7b41c579ee8a86c81a
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

When a deadline task in non-contending state migrates to another CPU via `migrate_task_rq_dl()`, a call to `sub_running_bw()` is made without first updating the runqueue clock. This causes an assertion failure in `sub_running_bw()` when it calls `cpufreq_update_util()`, which in turn calls `rq_clock()` and expects the clock to have been updated. The bug is triggered on slow-switching systems with CONFIG_ACPI_CPPC_CPUFREQ_FIE enabled and schedutil governor, when the `cppc_fie` DL task migrates.

## Root Cause

The `migrate_task_rq_dl()` function directly calls `sub_running_bw()` for non-contending tasks without first updating the runqueue clock. The `sub_running_bw()` function eventually invokes `cpufreq_update_util()`, which requires the runqueue clock to have been updated (checked via `assert_clock_updated()`). Without this update, the assertion fails, generating a kernel warning.

## Fix Summary

The fix adds a single `update_rq_clock(rq)` call before `sub_running_bw(&p->dl, &rq->dl)` in the non-contending branch of `migrate_task_rq_dl()`. This ensures the runqueue clock is properly updated before any downstream functions that depend on an up-to-date clock are called.

## Triggering Conditions

The bug occurs when a deadline task in non-contending state migrates between CPUs via `migrate_task_rq_dl()`. Specifically:
- A deadline task must be in `dl_non_contending` state (set when task blocks while consuming less bandwidth than reserved)
- The task must migrate to another CPU (via `set_task_cpu()` during wakeup or load balancing)
- The system must have CONFIG_ACPI_CPPC_CPUFREQ_FIE enabled with schedutil governor on slow-switching hardware
- During migration, `migrate_task_rq_dl()` calls `sub_running_bw()` without updating the runqueue clock first
- `sub_running_bw()` triggers `cpufreq_update_util()` which calls `rq_clock()` and expects an updated clock via `assert_clock_updated()`
- The assertion failure generates the kernel warning when clock update flags indicate the clock hasn't been updated

## Reproduce Strategy (kSTEP)

Create a deadline task that becomes non-contending, then force migration to trigger the bug:
- Use at least 2 CPUs (CPU 0 reserved for driver, migrate between CPU 1 and CPU 2)
- In `setup()`: Create a deadline task with `kstep_task_create()` and configure deadline parameters
- In `run()`: Wake the deadline task on CPU 1, let it run briefly then pause it to enter non-contending state
- Force task migration by changing CPU affinity or using `kstep_task_pin()` to move from CPU 1 to CPU 2
- Wake the task to trigger `migrate_task_rq_dl()` path during `try_to_wake_up()`
- Use `on_tick_begin()` callback to log task state and detect assertion failures in kernel logs
- Check for "clock_update_flags < RQCF_ACT_SKIP" warning message in dmesg or use kSTEP logging to detect the bug
- The bug is confirmed if kernel warning appears during task migration without proper clock update
