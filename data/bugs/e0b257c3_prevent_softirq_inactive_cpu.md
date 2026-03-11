# sched: Prevent raising SCHED_SOFTIRQ when CPU is !active

- **Commit:** e0b257c3b71bd98a4866c3daecf000998aaa4927
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c
- **Subsystem:** core

## Bug Description

When a CPU is being deactivated, it may remain in the `nohz.idle_cpus_mask` bitmap, allowing `SCHED_SOFTIRQ` to be raised on it. This causes a warning in NOHZ code when the tick is stopped and allows an inactive CPU to incorrectly participate in load balancing. The issue manifests as pending softirqs triggering warnings when stopping the tick on a deactivating CPU.

## Root Cause

The CPU was removed from the `nohz.idle_cpus_mask` only in `sched_cpu_dying()`, which occurs too late in the CPU shutdown sequence—after the CPU has already been marked inactive. Additionally, `trigger_load_balance()` did not check whether a CPU is active before raising `SCHED_SOFTIRQ`, allowing softirq invocation on inactive CPUs.

## Fix Summary

The fix moves the `nohz_balance_exit_idle()` call to `sched_cpu_deactivate()` (executed before `set_cpu_active(cpu, false)`) so the CPU is removed from the idle balancing mask at the correct time. Additionally, `trigger_load_balance()` is modified to check `!cpu_active(cpu_of(rq))` and prevent raising `SCHED_SOFTIRQ` when the CPU is not active.

## Triggering Conditions

The bug occurs during CPU teardown when an inactive CPU remains in `nohz.idle_cpus_mask` and continues to participate in load balancing. The scheduler tick (`scheduler_tick()`) calls `trigger_load_balance()` which can raise `SCHED_SOFTIRQ` on the deactivating CPU. This happens between the time when `set_cpu_active(cpu, false)` marks the CPU inactive and when `sched_cpu_dying()` removes it from the nohz mask. The timing window requires:
- A CPU undergoing teardown/shutdown sequence
- The CPU reaching the deactivated state but not yet offline
- Scheduler tick firing on the deactivating CPU
- Load balancing conditions being met (`time_after_eq(jiffies, rq->next_balance)`)
- The deactivating CPU still present in `nohz.idle_cpus_mask`

When SCHED_SOFTIRQ remains pending as the tick stops, NOHZ code triggers a warning about pending softirqs during tick deactivation.

## Reproduce Strategy (kSTEP)

This bug requires simulating CPU hotplug behavior, which is challenging with standard kSTEP APIs since they don't directly control CPU activation state. A reproduction approach would need to:
- Set up at least 2 CPUs (CPU 1 for normal operation, simulate issues on CPU 2+)
- Create tasks to generate scheduling activity and load balancing triggers
- Use `kstep_tick_repeat()` to advance time and trigger periodic load balancing
- Monitor `on_sched_softirq_begin/end` callbacks to detect SCHED_SOFTIRQ activity
- The bug requires kernel-level CPU hotplug simulation or direct manipulation of `cpu_active_mask` and `nohz.idle_cpus_mask`, which may require extending kSTEP
- Detection would involve checking if SCHED_SOFTIRQ is raised on an inactive CPU or observing warnings in NOHZ tick stopping logic
- Use timing-based checks with `time_after_eq(jiffies, rq->next_balance)` conditions to ensure load balancing triggers are met

A complete reproduction likely requires direct kernel state manipulation beyond current kSTEP capabilities.
