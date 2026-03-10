# Fix unbalance set_rq_online/offline() in sched_cpu_deactivate()

- **Commit:** fe7a11c78d2a9bdb8b50afc278a31ac177000948
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

During CPU deactivation, `set_rq_offline()` is called to take the run queue offline. If `cpuset_cpu_inactive()` fails and the error handling path is triggered, the run queue is left in the offline state while other state changes are rolled back. This creates an imbalance between the offline and online calls, leaving the CPU's run queue in an incorrect state that may cause subsequent scheduler operations to behave incorrectly.

## Root Cause

The error handling path in `sched_cpu_deactivate()` rolls back the SMT present counter and other operations when `cpuset_cpu_inactive()` fails, but it was missing the corresponding `sched_set_rq_online()` call to undo the earlier `set_rq_offline()`. This asymmetry leaves the run queue offline when it should be brought back online during the rollback.

## Fix Summary

The fix adds a call to `sched_set_rq_online(rq, cpu)` in the error handling path of `sched_cpu_deactivate()` when `cpuset_cpu_inactive()` fails, ensuring the run queue is brought back online to match the earlier offline call. This restores the balance between online and offline operations.

## Triggering Conditions

The bug triggers during CPU hotplug deactivation when the following sequence occurs:
- CPU deactivation is initiated via `sched_cpu_deactivate()` 
- The run queue is successfully taken offline with `sched_set_rq_offline(rq, cpu)`
- SMT present counter is decremented via `sched_smt_present_dec(cpu)`
- The `cpuset_cpu_inactive(cpu)` call fails and returns an error
- Error handling rolls back SMT counter and other state but leaves run queue offline
- The CPU's run queue remains in offline state while other scheduler state indicates online
- Subsequent scheduler operations may encounter inconsistent run queue state

## Reproduce Strategy (kSTEP)

This bug involves CPU hotplug infrastructure that cannot be directly reproduced through standard kSTEP task operations. To simulate the bug conditions:
- Use 2+ CPUs (CPU 0 reserved for driver, test on CPU 1+)
- Create a mock scenario where a CPU's run queue is artificially set offline via direct kernel manipulation
- Use kernel-side logging to track run queue online/offline state during operations
- Monitor run queue state before and after operations that expect consistent online state
- Create tasks pinned to the affected CPU and observe scheduling behavior inconsistencies
- Use `on_tick_begin()` callback to log run queue state and detect imbalance
- Verify that tasks cannot be properly scheduled to the CPU with inconsistent state
- Check for scheduler warnings or incorrect load balancing behavior due to offline run queue
