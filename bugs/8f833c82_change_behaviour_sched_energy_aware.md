# sched/topology: Change behaviour of the 'sched_energy_aware' sysctl, based on the platform

- **Commit:** 8f833c82cdab7b4049bcfe88311d35fa5f24e422
- **Affected file(s):** kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

The 'sched_energy_aware' sysctl returns 1 (enabled) on platforms that do not support Energy-Aware Scheduling (EAS), even though EAS is not actually possible on those platforms. This confuses administrators who see the sysctl enabled but EAS is not functioning. The sysctl also incorrectly accepts writes on unsupported platforms, calling build_perf_domains() when it should return an error.

## Root Cause

The sysctl handler `sched_energy_aware_handler()` did not check whether EAS is actually possible on the platform before returning values or accepting writes. The EAS capability checks (asymmetric CPU capacity, no SMT, schedutil governor, frequency invariance) were only performed inside `build_perf_domains()`, leaving the sysctl interface to report a misleading state.

## Fix Summary

The fix extracts EAS capability checking into a new function `sched_is_eas_possible()` and calls it from the sysctl handler. On unsupported platforms, writes now return -EOPNOTSUPP error and reads return empty result. This ensures the sysctl accurately reflects whether EAS is actually possible on the platform.

## Triggering Conditions

The bug manifests when accessing `/proc/sys/kernel/sched_energy_aware` on platforms that don't support EAS. The sysctl handler `sched_energy_aware_handler()` incorrectly returns success for both reads (showing "1") and writes without validating EAS requirements. EAS requires: asymmetric CPU capacity, no SMT, schedutil governor, and frequency invariance. On platforms missing any requirement (e.g., symmetric topology, SMT enabled, non-schedutil governor), the sysctl should return empty for reads and -EOPNOTSUPP for writes, but incorrectly shows "1" and accepts writes that call `build_perf_domains()` unnecessarily.

## Reproduce Strategy (kSTEP)

Create a topology that violates EAS requirements to trigger the buggy sysctl behavior:
- Use 2+ CPUs (CPU 0 reserved) with symmetric capacity via `kstep_cpu_set_capacity()`
- Set up SMT topology using `kstep_topo_set_smt()` to violate the "no SMT" requirement  
- In `setup()`: call `kstep_topo_init()`, configure symmetric CPUs 1-4 with same capacity, set SMT pairs `["0", "1-2", "1-2", "3-4", "3-4"]`, then `kstep_topo_apply()`
- In `run()`: read `/proc/sys/kernel/sched_energy_aware` via `kstep_write("/proc/sys/kernel/sched_energy_aware", "1", 1)` and observe it succeeds instead of returning -EOPNOTSUPP
- Check that `build_perf_domains()` gets called unnecessarily by monitoring EAS domain setup
- On buggy kernels: sysctl shows "1" and accepts writes; on fixed kernels: returns empty/error
