# sched/smt: Fix unbalance sched_smt_present dec/inc

- **Commit:** e22f910a26cc2a3ac9c66b8e935ef2a7dd881117
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** sched/smt

## Bug Description

The bug manifests as a kernel warning "jump label: negative count!" when CPU offline fails after partial completion. Specifically, when `sched_cpu_deactivate()` decrements `sched_smt_present` but `cpuset_cpu_inactive()` subsequently fails, the CPU offline operation is aborted. However, the counter remains decremented while the CPU remains online, causing an unbalanced state between the counter and actual topology.

## Root Cause

In `sched_cpu_deactivate()`, `sched_smt_present_dec(cpu)` is called early in the function to decrement the SMT presence counter. If `cpuset_cpu_inactive(cpu)` later fails and the function returns an error, the CPU offline is aborted and the CPU remains online. However, since the counter was already decremented before the failure point, the counter becomes out of sync with the actual topology, leading to a negative count when attempting to decrement again in a future offline attempt.

## Fix Summary

The fix adds `sched_smt_present_inc(cpu)` in the error path when `cpuset_cpu_inactive()` fails, restoring the counter to its correct value before returning the error. This ensures the counter remains balanced with the actual CPU topology regardless of whether the deactivation succeeds or fails.

## Triggering Conditions

This bug requires an SMT-enabled system where CPU hotplug operations can partially fail. The specific conditions needed are:

- A multi-threaded system with SMT topology (hyperthreading enabled)
- CPU offline operation that calls `sched_cpu_deactivate()` which decrements `sched_smt_present`  
- The `cpuset_cpu_inactive()` call within `sched_cpu_deactivate()` must fail after the counter decrement
- A subsequent CPU offline attempt on the same or related CPU, causing another decrement on an already-decremented counter
- This creates a negative count in the jump label subsystem, triggering the "negative count!" warning in `static_key_slow_try_dec()`

The race occurs in the error path of `sched_cpu_deactivate()` where cleanup operations don't properly restore the SMT presence counter.

## Reproduce Strategy (kSTEP)

Since kSTEP operates at the scheduler level and cannot directly trigger CPU hotplug failures, this bug is challenging to reproduce through normal kSTEP drivers. However, we can simulate the core issue by:

- **Setup**: Use at least 2 CPUs (CPU 0 reserved, CPUs 1+ for testing). Configure SMT topology with `kstep_topo_init()` and `kstep_topo_set_smt()` to create sibling CPU pairs
- **Manual Counter Manipulation**: If kernel allows, directly call `sched_smt_present_dec()` to simulate the early decrement in `sched_cpu_deactivate()` 
- **State Verification**: Use callback functions like `on_tick_begin()` to monitor the SMT presence counter state and detect imbalances
- **Detection Method**: Check for negative values in the SMT counter or import kernel symbols to verify `sched_smt_present` state
- **Alternative**: Monitor kernel logs via `printk` injection to detect "jump label: negative count!" warnings during scheduler operations

Note: Full reproduction may require kernel module capabilities beyond standard kSTEP framework to trigger actual CPU hotplug failures.
