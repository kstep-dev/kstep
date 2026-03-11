# sched/debug: Fix fair_server_period_max value

- **Commit:** 4ae0c2b91110dab6f4291c2c7f99dde60ecc97d8
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** CFS (Fair)

## Bug Description

The `fair_server_period_max` variable is initialized with an integer overflow bug caused by using a signed `int` in the left-shift operation. This results in the variable being set to 0xfffffffffa000000 (approximately 585 years) instead of the intended 0xfa000000 (approximately 4 seconds). This incorrect maximum period value would allow the fair server to be configured with periods far exceeding the intended limit, potentially causing unexpected scheduler behavior.

## Root Cause

The expression `(1 << 22) * NSEC_PER_USEC` performs a signed integer left-shift on the literal `1` (which has type `int`), causing integer overflow. When this overflowed value is multiplied by `NSEC_PER_USEC` and assigned to the `unsigned long` variable, sign extension occurs, resulting in the wrong value being stored.

## Fix Summary

The fix changes `(1 << 22)` to `(1UL << 22)`, explicitly making the left-shift operation use an `unsigned long` instead of a signed `int`. This ensures the multiplication and assignment produce the correct value without overflow or sign extension issues.

## Triggering Conditions

This is a compile-time bug that manifests during kernel initialization when the `fair_server_period_max` variable is assigned its value. The bug occurs automatically in affected kernel versions when:
- The kernel/sched/debug.c file is compiled with gcc-13 or compilers that catch integer overflow
- The fair server debugging interface is enabled (CONFIG_SCHED_DEBUG=y)
- The incorrect value (0xfffffffffa000000) allows setting excessively large fair server periods via /sys/kernel/debug/sched/
- Any attempt to configure a fair server period through the debugfs interface would accept periods up to 585 years instead of the intended 4-second maximum

## Reproduce Strategy (kSTEP)

This bug is a compile-time constant initialization issue rather than a runtime scheduler behavior bug. The manifestation requires:
- 2+ CPUs (CPU 0 reserved for driver)
- Access to /sys/kernel/debug/sched/ to observe the incorrect maximum value
- Create a driver that reads the fair_server_period_max value directly from kernel memory or attempts to set an invalid period
- In setup(): Use `kstep_task_create()` to create test tasks on CPUs 1-2
- In run(): Access the compiled-in `fair_server_period_max` value and verify it equals 0xfffffffffa000000 (buggy) vs 0xfa000000 (fixed)
- Use kernel symbol lookup or direct memory access to read the global variable
- Log the actual vs expected values to detect the integer overflow bug
- Alternatively, attempt to write a 5-second period via debugfs, which should fail on fixed kernels but succeed on buggy ones
