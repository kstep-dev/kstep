# sched/isolation: Fix boot crash when maxcpus < first housekeeping CPU

- **Commit:** 257bf89d84121280904800acd25cc2c444c717ae
- **Affected file(s):** kernel/sched/isolation.c
- **Subsystem:** core (housekeeping/isolation)

## Bug Description

The kernel crashes at boot time when the `maxcpus=` parameter limits available CPUs below the first housekeeping CPU specified in `nohz_full=` or `isolcpus=` parameters. For example, with `maxcpus=2 nohz_full=0-2` on a 4-CPU virtual machine, the kernel fails to ensure a valid housekeeping CPU exists for the system to function. Additionally, edge cases like `nohz_full=0` on single-CPU systems result in an empty non-housekeeping mask, causing warnings and boot failures.

## Root Cause

The `housekeeping_setup()` function checked if housekeeping CPUs exist using `cpumask_intersects(cpu_present_mask, housekeeping_staging)`, which only verifies that a CPU is physically present, not whether it is within the `setup_max_cpus` limit imposed by the `maxcpus=` parameter. This mismatch causes the kernel to configure housekeeping on a CPU that will never be online after boot, leaving no valid housekeeping CPU. Additionally, invalid `nohz_full=` parameters that parse to empty masks were not rejected, causing configuration inconsistencies.

## Fix Summary

The fix replaces the intersection check with `cpumask_first_and()` and explicitly validates that the first matching CPU is less than `setup_max_cpus`. It also adds an early check to silently ignore empty non-housekeeping masks, preventing invalid configurations from proceeding.

## Triggering Conditions

The bug occurs during early boot when `housekeeping_setup()` is called with CPU isolation parameters. Two specific scenarios trigger crashes: (1) `maxcpus=N` parameter limits available CPUs below the first housekeeping CPU in the `nohz_full=` or `isolcpus=` mask (e.g., `maxcpus=2 nohz_full=0-2`), causing the intersection check to pass but leaving no valid housekeeping CPU within `setup_max_cpus`; (2) Empty non-housekeeping masks from invalid parameters like `nohz_full=0` on single-CPU systems, causing tick_nohz_full_setup() to trigger `WARN_ON(tick_nohz_full_running)` in `tick_sched_do_timer()`.

## Reproduce Strategy (kSTEP)

This is a boot-time initialization bug that occurs before the scheduler is fully running, making direct kSTEP reproduction challenging. However, we can test the fixed logic by simulating the housekeeping setup conditions. Use 2+ CPUs, create a mock scenario in `setup()` that calls the internal housekeeping validation logic with manipulated `setup_max_cpus` and CPU masks. In `run()`, use `kstep_sysctl_write()` to configure isolation parameters and `kstep_tick_repeat(1)` to trigger housekeeping checks. Monitor kernel logs with `on_tick_begin()` callback for WARN_ON messages. Check if the system maintains at least one valid housekeeping CPU by examining the housekeeping cpumasks and verifying no crashes occur during tick processing.
