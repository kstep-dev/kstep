# sched/fair: Fix race between runtime distribution and assignment

- **Commit:** 26a8b12747c975b33b4a82d62e4a307e1c07f31b
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

A race condition exists between `distribute_cfs_runtime()` and `assign_cfs_rq_runtime()` in the CFS bandwidth control mechanism. When bandwidth is distributed to throttled CFS run queues, `cfs_b->runtime` is read and distribution proceeds without holding the lock. Concurrently, `assign_cfs_rq_runtime()` may be called and access `cfs_b->runtime` at the same time, causing the runtime budget to be over-allocated. Real-world testing with fibtest showed approximately 70% over-use of runtime quota (17% CPU usage instead of 10% on a 96-core machine).

## Root Cause

The `distribute_cfs_runtime()` function reads `cfs_b->runtime` and distributes it to throttled queues without holding `cfs_b->lock`. The caller temporarily releases the lock to avoid nesting, but during this window, other code paths (such as `assign_cfs_rq_runtime()`) can modify `cfs_b->runtime` concurrently, creating a race where both paths consume the same runtime budget, leading to bandwidth control violations.

## Fix Summary

The fix moves the lock acquisition inside `distribute_cfs_runtime()` so that each runtime allocation is protected atomically. Instead of passing the runtime amount as a parameter and returning the amount distributed, the function now checks `cfs_b->runtime` directly under the lock for each throttled queue, ensuring that the runtime accounting is always protected and preventing double-allocation of the same runtime budget.

## Triggering Conditions

The race occurs in the CFS bandwidth control subsystem during period timer execution. Key conditions:
- Multiple throttled CFS run queues exist across CPUs with tasks requiring runtime
- CFS period timer fires, triggering `do_sched_cfs_period_timer()` → `distribute_cfs_runtime()`
- Concurrently, `assign_cfs_rq_runtime()` is called (e.g., from task wakeup or tick processing)
- The caller temporarily releases `cfs_b->lock` before calling `distribute_cfs_runtime()` to avoid lock nesting
- During this unlocked window, both paths read and modify `cfs_b->runtime` simultaneously
- Timing-sensitive: requires tasks returning unused runtime while distribution occurs
- Race window is narrow but can cause significant over-allocation (up to 70% in real workloads)

## Reproduce Strategy (kSTEP)

Setup requires 3+ CPUs (CPU 0 reserved for driver) and CFS bandwidth control:
- In `setup()`: Create bandwidth-limited cgroup with `kstep_cgroup_create("test")` and `kstep_cgroup_write("test", "cpu.cfs_quota_us", "10000")` (10ms quota per 100ms period)
- Create multiple tasks with `kstep_task_create()` and assign to cgroup via `kstep_cgroup_add_task()`
- Pin tasks to different CPUs (1, 2, 3) using `kstep_task_pin()` to create multiple throttled run queues
- In `run()`: Use `kstep_tick_repeat()` to exhaust quota and throttle all cgroups
- Trigger period timer with precise timing using `kstep_sleep()` for period boundary
- Use `on_tick_begin()` callback to monitor `cfs_b->runtime` values and detect over-allocation
- Log runtime before/after distribution and compare with expected quota to detect the race
- Success: runtime consumption exceeds configured quota (>10ms in 100ms period)
