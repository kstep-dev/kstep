# sched/isolation: Prevent boot crash when the boot CPU is nohz_full

- **Commit:** 5097cbcb38e6e0d2627c9dde1985e91d2c9f880e
- **Affected file(s):** kernel/sched/isolation.c
- **Subsystem:** isolation

## Bug Description

When the boot CPU is configured as nohz_full, the kernel crashes at boot time because `housekeeping_any_cpu()` returns an invalid CPU number (>= nr_cpu_ids). This occurs before the first housekeeping CPU comes online during early boot, causing subsequent code (particularly workqueue timer setup) to fail when it tries to use this invalid CPU number. The crash prevents the system from booting entirely.

## Root Cause

The function `housekeeping_any_cpu()` directly returns the result of `cpumask_any_and(housekeeping.cpumasks[type], cpu_online_mask)` without validating it. At boot time, before `smp_init()` brings the first housekeeping CPU online, when all CPUs are isolated except the boot CPU, the function can return an invalid CPU number (>= nr_cpu_ids) to the workqueue code, which then crashes when attempting to use it.

## Fix Summary

The fix adds validation of the `cpumask_any_and()` result. If the result is invalid (>= nr_cpu_ids), the function returns `smp_processor_id()` (the boot CPU) instead, which is always valid and online. A WARN_ON_ONCE ensures the function only falls through to this path at boot time for timer-related operations, catching any unexpected cases.

## Triggering Conditions

This bug occurs during kernel boot when:
- Boot CPU (CPU 0) is configured as `nohz_full` (via boot parameter like `nohz_full=0`)
- All other CPUs are also configured as `nohz_full`, making them isolated
- `housekeeping_any_cpu(HK_TYPE_TIMER)` is called before `smp_init()` brings first housekeeping CPU online
- The housekeeping cpumask contains no online CPUs, causing `cpumask_any_and()` to return an invalid CPU >= nr_cpu_ids
- Workqueue timer setup code attempts to use this invalid CPU number and crashes
- Timing-sensitive: occurs during early boot before secondary CPUs are brought online

## Reproduce Strategy (kSTEP)

Reproducing this bug requires simulating early boot conditions with isolation configuration:
- Setup: Configure CPU 0 as isolated (`nohz_full`) with all other CPUs also isolated initially
- Use `kstep_sysctl_write()` to set `kernel.nohz_full` or equivalent isolation parameters
- Create a scenario where no housekeeping CPUs are online in cpu_online_mask
- Call workqueue-related functions that trigger `housekeeping_any_cpu(HK_TYPE_TIMER)`
- Monitor for invalid CPU numbers returned (>= nr_cpu_ids) in callbacks
- Use `on_tick_begin()` callback to log CPU states and isolation configuration
- Detection: Check for kernel panic/crash when invalid CPU used for timer setup
- Verify fix by ensuring fallback to `smp_processor_id()` with WARN_ON_ONCE trigger
