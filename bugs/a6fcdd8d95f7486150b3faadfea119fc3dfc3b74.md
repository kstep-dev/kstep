# sched/debug: Correct printing for rq->nr_uninterruptible

- **Commit:** a6fcdd8d95f7486150b3faadfea119fc3dfc3b74
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core (scheduler debug)

## Bug Description

When `rq->nr_uninterruptible` (type: `unsigned int`) was printed using a `long` cast and `%ld` format specifier, the debug output displayed incorrect values. For example, a value of `0xfffffff7` (4,294,967,287 as unsigned int) would be printed as 4,294,967,287 instead of the correctly interpreted signed value of -9. This resulted in misleading debug output in `/sys/kernel/debug/sched/debug` and console logs.

## Root Cause

A previous commit (e6fe3f422be1) changed the type of `rq->nr_uninterruptible` from `unsigned long` to `unsigned int`, but the print statement in the debug macro was not updated. The code continued to use `(long)` cast with `%ld` format specifier, causing incorrect interpretation and display of 32-bit unsigned values when printed as 64-bit signed longs.

## Fix Summary

Changed the cast and format specifier from `(long)` and `%ld` to `(int)` and `%d` for 4-byte fields in the debug print macro. This ensures that 32-bit unsigned integers are correctly cast and printed with the appropriate format specifier.
