# sched_ext: Add a missing newline at the end of an error message

- **Commit:** f7d1b585e1533e26801c13569b96b84b2ad2d3c1
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The error message printed when sched_ext is incompatible with isolcpus domain isolation lacks a newline terminator. This causes the error message to not be properly flushed or displayed in kernel logs, potentially appearing truncated or merged with subsequent output on the same line. Users attempting to enable sched_ext with isolcpus would encounter improperly formatted error messages that are difficult to read.

## Root Cause

The `pr_err()` macro call omitted the `\n` newline character at the end of the error message string. In the Linux kernel's logging system, messages should be properly terminated with newlines to ensure correct formatting, flushing, and display in kernel logs. Without the newline, the kernel's log buffer may not properly handle the message boundary.

## Fix Summary

The fix adds the missing `\n` newline character to the end of the error message string. This ensures the error message is properly formatted and displayed in kernel logs, making diagnostic output clearer for users encountering the isolcpus incompatibility.

## Triggering Conditions

The bug is triggered when attempting to enable sched_ext operations on a system configured with `isolcpus=` domain isolation. Specifically, the error occurs in `scx_ops_enable()` when `housekeeping_cpumask(HK_TYPE_DOMAIN)` is not equal to `cpu_possible_mask`, indicating that some CPUs are isolated from scheduling domains. This happens during the validation phase before sched_ext becomes active. The timing is deterministic - the error message is printed immediately when the incompatibility is detected, but without the trailing newline, subsequent kernel log output may appear on the same line.

## Reproduce Strategy (kSTEP)

This is a formatting bug rather than a functional scheduler bug, so it cannot be directly reproduced through kSTEP's runtime simulation. The issue occurs at sched_ext initialization time when the kernel detects `isolcpus=` configuration, which is a boot-time parameter that affects the housekeeping subsystem. kSTEP operates within an already-booted kernel environment where CPU isolation configuration is fixed. The bug would need to be reproduced by:
1. Booting with `isolcpus=` domain isolation parameter
2. Attempting to load a sched_ext BPF program
3. Observing the malformed error message in kernel logs (missing newline)
This type of boot-parameter and BPF program interaction is outside kSTEP's simulation scope.
