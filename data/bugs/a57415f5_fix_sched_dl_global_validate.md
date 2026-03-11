# Fix sched_dl_global_validate()

- **Commit:** a57415f5d1e43c3a5c5d412cd85e2792d7ed9b11
- **Affected file(s):** kernel/sched/deadline.c, kernel/sched/sched.h
- **Subsystem:** Deadline (SCHED_DEADLINE)

## Bug Description

The sched_dl_global_validate() function incorrectly compares per-CPU bandwidth against per-root-domain total bandwidth. This causes incorrect admission control decisions when validating whether new sched_rt_{runtime,period}_us settings can accommodate currently allocated DEADLINE bandwidth. The bug may allow tasks to be admitted beyond the system's actual capacity, or incorrectly reject valid settings.

## Root Cause

The bandwidth comparison uses a per-CPU `new_bw` value directly against `dl_b->total_bw`, which represents the total allocated bandwidth across all CPUs in a root domain. Under CONFIG_SMP, dl_bw is per-root-domain, not per-CPU. The fix requires multiplying `new_bw` by the number of CPUs in the root domain to obtain the total available bandwidth for proper comparison.

## Fix Summary

The fix adds a per-root-domain CPU count multiplier in the bandwidth validation. The comparison is changed from `new_bw < dl_b->total_bw` to `new_bw * cpus < dl_b->total_bw`, where `cpus` is obtained via `dl_bw_cpus(cpu)`. The fix also updates the struct dl_bw documentation to clarify that bandwidth is per-root-domain, not per-CPU.

## Triggering Conditions

This bug triggers during sysctl modification of `sched_rt_{runtime,period}_us` parameters through the sched_rt_handler() -> sched_dl_bandwidth_validate() -> sched_dl_global_validate() path. The system must have CONFIG_SMP enabled and multiple CPUs configured. The bug manifests when DEADLINE tasks with accumulated bandwidth (dl_b->total_bw) exist in a root domain containing multiple CPUs. The validation incorrectly compares per-CPU `new_bw` against root-domain `total_bw`, causing false rejections when `new_bw < total_bw` but `new_bw * num_cpus >= total_bw` should be valid. The root domain must contain >1 CPU for the bug to affect behavior.

## Reproduce Strategy (kSTEP)

Configure a multi-CPU system (≥3 CPUs, excluding driver CPU 0) using kstep_topo_* functions. In setup(), create DEADLINE tasks with significant bandwidth allocation using kstep_task_create() and configure them with appropriate deadline parameters via direct task_struct manipulation or syscall emulation. Place tasks across multiple CPUs within the same root domain to accumulate total_bw. In run(), modify sched_rt_runtime_us via kstep_sysctl_write() to trigger sched_dl_global_validate(). Monitor the validation result using callbacks and kernel logs. The bug is detected when valid configurations (where RT bandwidth still exceeds total DL bandwidth across all CPUs) are incorrectly rejected with -EBUSY. Use TRACE_INFO() to log new_bw, total_bw, and CPU count values during validation.
