# Revert "cpufreq: CPPC: Add support for frequency invariance"

- **Commit:** 771fac5e26c17845de8c679e6a947a4371e86ffc
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The frequency invariance support for the CPPC driver introduced in a previous commit contains race conditions where the driver fails to properly stop kthread_work and irq_work on policy exit during suspend/resume or CPU hotplug operations. This can lead to use-after-free, resource leaks, or execution of stale work items after the policy has been freed.

## Root Cause

The CPPC driver's frequency invariance implementation does not properly clean up asynchronous work (kthread_work and irq_work) when the frequency scaling policy exits. Without proper synchronization and cleanup on suspend/resume or CPU hotplug, these work items may reference freed memory or execute in an inconsistent scheduler state.

## Fix Summary

The commit reverts the entire problematic frequency invariance support for CPPC by removing the export symbol that was added for that feature. A proper fix would require significant refactoring of the cleanup paths, which was not feasible for the 5.13-rc release window, so the feature is reverted instead.

## Triggering Conditions

The bug occurs in the CPPC frequency invariance subsystem when the cpufreq policy exits during CPU hotplug or suspend/resume operations. The critical race involves:
- Active kthread_worker (`kworker_fie`) and per-CPU irq_work items still executing
- cppc_scale_freq_tick() scheduling irq_work via irq_work_queue() from scheduler tick context  
- Concurrent policy exit calling cppc_freq_invariance_exit() without proper synchronization
- kthread_work items (cppc_scale_freq_workfn) accessing freed cppc_cpudata structures
- Timing window where irq_work executes after policy data structures are deallocated

## Reproduce Strategy (kSTEP)

Reproducing this race requires simulating concurrent CPPC frequency invariance operations with policy exit:
- Use 2+ CPUs (CPU 0 reserved for driver) 
- In setup(): Enable ACPI_CPPC_CPUFREQ_FIE config, create cpufreq policy via kstep_sysctl_write()
- Create mock CPPC driver state with kstep_task_create() representing frequency scaling threads
- In run(): Use kstep_tick_repeat() to trigger multiple scheduler ticks (simulating cppc_scale_freq_tick calls)
- Concurrently simulate policy exit with kstep_task_pause() on scaling tasks
- Use on_tick_begin callback to log kthread_worker and irq_work queue states
- Use on_sched_softirq_end callback to detect use-after-free via checking cppc_cpudata access patterns
- Trigger race by rapid tick/pause cycles: kstep_tick() followed immediately by task state changes
- Detect bug via kernel log messages showing work queue corruption or use-after-free access
