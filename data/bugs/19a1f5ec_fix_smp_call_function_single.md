# sched: Fix smp_call_function_single_async() usage for ILB

- **Commit:** 19a1f5ec699954d21be10f74ff71c2a7079e99ad
- **Affected file(s):** kernel/sched/core.c, kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** core scheduler (nohz idle load balancing)

## Bug Description

The previous commit 90b5363acd47 introduced improper serialization of the CSD (call site data) structure used in `smp_call_function_single_async()` within the idle load balancer (ILB) mechanism. The CSD is reused across multiple IPI calls, but the serialization guarantees were not met, potentially allowing the same CSD to be queued multiple times. This race condition could cause memory corruption or incorrect IPI handling during idle load balancing operations on systems with no-HZ scheduling enabled.

## Root Cause

While `kick_ilb()` had an atomic test-and-set operation for the NOHZ_KICK_MASK flag, the matching release operation in the old `got_nohz_idle_kick()` was not in the right place to guarantee serialization of the CSD. The atomic acquire and release were not properly paired, creating a window where another CPU could re-use the same CSD before the previous IPI had fully completed, violating the serialization requirements of `smp_call_function_single_async()`.

## Fix Summary

The fix moves the CSD release operation into the IPI callback (`nohz_csd_func()`) where it is executed atomically with the IPI processing. A new `nohz_idle_balance` field is added to the runqueue structure to communicate the flags from the IPI handler to the balance logic, ensuring the atomic acquire-release pair properly guards the CSD throughout its lifetime.

## Triggering Conditions

This race condition occurs in the nohz idle load balancing (ILB) mechanism when multiple CPUs concurrently attempt to trigger idle load balancing on the same idle CPU. The bug requires:

- A system with CONFIG_NO_HZ_COMMON enabled and multiple CPUs where some CPUs can enter idle states
- CPU topology where one CPU becomes the designated idle load balancer (ILB CPU)
- Multiple CPUs simultaneously detecting load imbalance and calling `kick_ilb()` with the same target ILB CPU
- The race window occurs between `atomic_fetch_or(flags, nohz_flags(ilb_cpu))` in `kick_ilb()` and the completion of the IPI callback `nohz_csd_func()`
- Before the fix, the CSD release in `got_nohz_idle_kick()` happened outside the IPI context, allowing the same CSD structure to be requeued via `smp_call_function_single_async()` while still in use
- The timing must allow the second `kick_ilb()` call to pass the `flags & NOHZ_KICK_MASK` check before the previous IPI's `got_nohz_idle_kick()` clears the flags

## Reproduce Strategy (kSTEP)

To reproduce this CSD serialization race, create a multi-CPU scenario that triggers concurrent idle load balancing requests:

- Use at least 4 CPUs (CPU 0 reserved for driver, CPUs 1-3+ for reproducing the race)
- Create an imbalanced load scenario: pin multiple tasks to CPU 1, leaving CPUs 2-3 idle to become ILB candidates
- Use `kstep_task_create()` and `kstep_task_pin()` to create 3+ tasks all pinned to CPU 1
- Configure CPU topology with `kstep_topo_init()` and `kstep_topo_apply()` to ensure proper scheduling domain hierarchy
- In the `run()` function, create load imbalance by running `kstep_tick_repeat(100)` to establish the scenario
- Then rapidly trigger multiple load balancing opportunities by changing task affinities with `kstep_task_pin()` to allow migration to idle CPUs
- Use `on_sched_softirq_begin()` and `on_sched_softirq_end()` callbacks to monitor nohz_idle_balance invocations
- Monitor for memory corruption indicators or inconsistent IPI callback states
- Check for WARN_ON triggers or kernel panics that would indicate CSD double-queuing
- The bug manifests as potential memory corruption or inconsistent scheduler state when the same CSD is queued multiple times
